#include <ATen/core/ivalue.h>
#include <c10/util/Exception.h>
#include <caffe2/serialize/file_adapter.h>
#include <caffe2/serialize/inline_container.h>
#include <torch/csrc/jit/mobile/backport_manager.h>
#include <torch/csrc/jit/mobile/import.h>
#include <torch/csrc/jit/mobile/model_compatibility.h>
#include <torch/csrc/jit/mobile/module.h>
#include <torch/csrc/jit/serialization/import.h>
#include <torch/csrc/jit/serialization/export.h>
#include <torch/csrc/jit/serialization/pickler.h>
#include <cstddef>

namespace torch {
namespace jit {

using caffe2::serialize::FileAdapter;
using caffe2::serialize::IStreamAdapter;
using caffe2::serialize::PyTorchStreamReader;
using caffe2::serialize::PyTorchStreamWriter;
using caffe2::serialize::ReadAdapterInterface;

// Current support bytecode version
namespace {
constexpr int64_t kBytecodeVersionV4 = 0x4L;
constexpr int64_t kBytecodeVersionV5 = 0x5L;
constexpr int64_t kBytecodeVersionV6 = 0x6L;
} // namespace

// Utility function that can be reused by backport_vn_to_vn-1(). If any utility
// function can be reused by other backport function, move it here.
namespace {
bool update_bytecode_version(
    std::vector<at::IValue>& bytecode_values,
    const int64_t to_version) {
  if (!bytecode_values.empty() && bytecode_values[0].isInt()) {
    bytecode_values[0] = c10::IValue(to_version);
    return true;
  }
  return false;
}

// Copy files from source to destination except the files and dirs
void selective_copy(
    PyTorchStreamReader& reader,
    PyTorchStreamWriter& writer,
    const std::unordered_set<std::string>& excluded_files,
    const std::unordered_set<std::string>& excluded_dirs) {
  auto records = reader.getAllRecords();
  for (const auto& record : records) {
    // Don't copy archive in excluded_files, usually archive `version` and
    // `bytecode`. Archvie `version` will be written when PyTorchStreamWriter is
    // going to finalize and run writeEndOfFile()

    // records is the list of all files names in the zip file, and each record
    // is one file with path to parent folder, the example records is:
    // data.pkl
    // code/__torch__/___torch_mangle_5.py
    // code/__torch__/___torch_mangle_5.py.debug_pkl
    // constants/140245072983168.storage
    // constants.pkl
    // bytecode.pkl
    // version
    bool skip = false;

    // Skip files (exaxt path)
    for (const auto& excluded_file : excluded_files) {
      if (record == excluded_file) {
        skip = true;
        break;
      }
    }

    // Skip dirs, find the last '/' and compare it with record
    for (const auto& excluded_dir : excluded_dirs) {
      std::size_t found = record.find_last_of("/\\");
      auto path = record.substr(0, found);
      if (excluded_dir == path) {
        skip = true;
        break;
      }
    }
    if (!skip) {
      auto data_ptr = reader.getRecord(record);
      auto data = std::get<0>(data_ptr).get();
      auto size = std::get<1>(data_ptr);
      writer.writeRecord(record, data, size);
    }
  }
}

bool check_bytecode_version(
    const std::vector<c10::IValue>& bytecode_values,
    const int64_t expect_bytecode_version) {
  if (bytecode_values.empty()) {
    TORCH_WARN("Empty bytecode archive.");
    return false;
  } else if (bytecode_values[0] != expect_bytecode_version) {
    TORCH_WARN(
        "Expect bytecode version ",
        expect_bytecode_version,
        ", but it gets ",
        bytecode_values[0]);
    return false;
  }
  return true;
}

} // namespace

// To add next backport
// function, for example, backport_vn_to_vn-1, create an anonymous namespace
// with a backport_vn_to_vn-1 function + other necessary customized function. If
// a function can be reused by other backport functions, move it to the utility
// function group. It will be easier to split out backport_manager.cpp to
// smaller files when it grows too long.

// The functions needed for backport model from v5 to v4.
namespace {

void writeArchiveV4(
    PyTorchStreamWriter& writer,
    const std::string& archive_name,
    const c10::IValue& value) {
  std::vector<char> data;

  // Vector to capture the run-time class types during pickling the IValues
  std::vector<c10::ClassTypePtr> memoizedClassTypes;
  Pickler data_pickle(
      [&](const char* buf, size_t size) {
        data.insert(data.end(), buf, buf + size);
      },
      nullptr,
      nullptr,
      &memoizedClassTypes);
  data_pickle.protocol();
  data_pickle.pushIValue(value);
  data_pickle.stop();
  size_t i = 0;
  std::string prefix = archive_name + "/";

  for (const auto& td : data_pickle.tensorData()) {
    WriteableTensorData writable_td = getWriteableTensorData(td);
    std::string fname = prefix + c10::to_string(i++);
    writer.writeRecord(fname, writable_td.data(), writable_td.sizeInBytes());
  }
  std::string fname = archive_name + ".pkl";
  writer.writeRecord(fname, data.data(), data.size());
}

bool backport_v5_to_v4(
    PyTorchStreamReader& reader,
    PyTorchStreamWriter& writer) {
  PyTorchStreamWriter writer_debug("/Users/chenlai/Documents/pytorch/reuse_constant/tmp/zip/model_v4_debug.ptl");
  // 1) read from archive `bytecode` archive
  std::vector<IValue> bytecode_values = get_bytecode_values(reader);
  if (!check_bytecode_version(bytecode_values, kBytecodeVersionV5)) {
    TORCH_WARN("Incorrect bytecode version for input model.");
    return false;
  }
  std::vector<IValue> constants_values =
      readArchive(kArchiveNameConstants, reader).toTuple()->elements();

  // 2) Copy everything to new output, except some specific files and dirs
  // (usually version, bytecode.pkl and bytecode folder are skipped)
  std::unordered_set<std::string> excluded_files{
      "constants.pkl",
      "bytecode.pkl",
      "version",
  };

  std::unordered_set<std::string> excluded_dirs{
      "constants",
      "bytecode",
  };
  selective_copy(reader, writer, excluded_files, excluded_dirs);
  selective_copy(reader, writer_debug, excluded_files, excluded_dirs);

  // 3) write `bytecode` archive
  // Update the bytecode version in bytecode.pkl
  update_bytecode_version(bytecode_values, kBytecodeVersionV4);
  // Construct the list of ivalues to a big tuple
  auto bytecode_tuple = c10::ivalue::Tuple::create(std::move(bytecode_values));
  // write `bytecode` archive
  writeArchiveV4(writer, kArchiveNameBytecode, bytecode_tuple);
  writeArchiveV4(writer_debug, kArchiveNameBytecode, bytecode_tuple);
  // write `constants` archive
  auto constants_tuple =
      c10::ivalue::Tuple::create(std::move(constants_values));
  writeArchiveV4(writer, kArchiveNameConstants, bytecode_tuple);
  writeArchiveV4(writer_debug, kArchiveNameConstants, bytecode_tuple);
  return true;
}

} // namespace

namespace {

void write_file(PyTorchStreamReader& reader) {
  PyTorchStreamWriter writer("/Users/chenlai/Documents/pytorch/reuse_constant/tmp/model_v6_debug.ptl");
  selective_copy(reader, writer, std::unordered_set<std::string>{"version"}, std::unordered_set<std::string>());
}

bool backport_v6_to_v5(
    PyTorchStreamReader& reader,
    PyTorchStreamWriter& writer) {
  // std::ostringstream out;
  // auto writer_func = [&](const void* buf, size_t nbytes) -> size_t {
  //   out.write(static_cast<const char*>(buf), nbytes);
  //   return !out ? 0 : nbytes;
  // };
  // PyTorchStreamWriter intermediate_writer(writer_func);
  // selective_copy(reader,
  //                intermediate_writer,
  //                std::unordered_set<std::string>{"version"},
  //                std::unordered_set<std::string>());
  // intermediate_writer.finalized();

  write_file(reader);
  Module torch_script = torch::jit::load("/Users/chenlai/Documents/pytorch/reuse_constant/tmp/model_v6_debug.ptl");

  ExtraFilesMap empty_map;
  // c10::optional<at::Device> device;
  // Module torch_script = torch::jit::load(out.str(), c10::nullopt);
  BytecodeWriteVersion = 5;
  ScriptModuleSerializer scriptModuleSerializer(writer);
  scriptModuleSerializer.serialize(torch_script, empty_map, true, false);
  BytecodeWriteVersion = 6;
  return true;
}
}

// A generic contract for backport logic to the previous bytecode version.
// Args:
// * PyTorchStreamReader has access to the input model from N bytecode version.
// * PyTorchStreamWriter has access to the output model backported to the
// previous N-1 bytecode version. Returns true if successful, false otherwise.
using BytecodeBackportFunction = std::function<bool(
    caffe2::serialize::PyTorchStreamReader&,
    caffe2::serialize::PyTorchStreamWriter&)>;

BackportManager::BackportManager() {
  registerBytecodeBackportFunction(kBytecodeVersionV5, backport_v5_to_v4);
  registerBytecodeBackportFunction(kBytecodeVersionV6, backport_v6_to_v5);
}

std::unordered_map<
    int64_t,
    std::function<bool(
        caffe2::serialize::PyTorchStreamReader&,
        caffe2::serialize::PyTorchStreamWriter&)>>&
BackportManager::bytecodeBackportFunctions() const {
  static std::unordered_map<
      int64_t,
      std::function<bool(
          caffe2::serialize::PyTorchStreamReader&,
          caffe2::serialize::PyTorchStreamWriter&)>>
      backport_functions;
  return backport_functions;
}

bool BackportManager::hasBytecodeBackportFunction(
    const int64_t from_version) const {
  return bytecodeBackportFunctions().count(from_version);
}

void BackportManager::registerBytecodeBackportFunction(
    const int64_t from_version,
    const BytecodeBackportFunction& backport_function) {
  TORCH_CHECK(
      !hasBytecodeBackportFunction(from_version),
      "Backporting from version ",
      from_version,
      " is already registered.");
  bytecodeBackportFunctions()[from_version] = backport_function;
}

// The main function to run backport from version n to version i.
// All models (file or buffer) will be converted stream first, and
// istream_adapter has access to it. During the backport process,
// the intermediate result will be stored with stream.
bool BackportManager::backport(
    std::shared_ptr<IStreamAdapter> istream_adapter,
    PyTorchStreamWriter& final_writer,
    int64_t from_version,
    int64_t to_version) const {
  PyTorchStreamReader start_reader(istream_adapter);

  if (from_version <= to_version) {
    TORCH_WARN(
        "backport donesn't support backporting model to new version. It's trying to backport from version ",
        from_version,
        " to version ",
        to_version);
    return false;
  }
  int64_t bytecode_version = from_version;
  std::ostringstream out;
  auto writer_func = [&](const void* buf, size_t nbytes) -> size_t {
    out.write(static_cast<const char*>(buf), nbytes);
    return !out ? 0 : nbytes;
  };

  std::istringstream iss;
  bool backport_success = true;

  while (bytecode_version > to_version) {
    // Read from intermediate writer result if ostream is not empty, otherwise
    // it means that it's the first time to backport and read from the source.
    bool in_intermediate = false;
    if (!out.str().empty()) {
      in_intermediate = true;
      iss = std::istringstream(out.str());
    }
    out.clear();

    PyTorchStreamWriter intermediate_writer(writer_func);

    if (!hasBytecodeBackportFunction(bytecode_version)) {
      return false;
    }

    // When it's the last backport process, write to the final destination
    // otherwise, export to the intermediate ostream.
    if (in_intermediate) {
      PyTorchStreamReader intermediate_reader(&iss);
      if (bytecode_version - 1 == to_version) {
        backport_success &= bytecodeBackportFunctions()[bytecode_version--](
            intermediate_reader, final_writer);
      } else {
        backport_success &= bytecodeBackportFunctions()[bytecode_version--](
            intermediate_reader, intermediate_writer);
      }
    } else {
      if (bytecode_version - 1 == to_version) {
        backport_success &= bytecodeBackportFunctions()[bytecode_version--](
            start_reader, final_writer);
      } else {
        backport_success &= bytecodeBackportFunctions()[bytecode_version--](
            start_reader, intermediate_writer);
      }
    }
  }
  return backport_success;
}

} // namespace jit
} // namespace torch
