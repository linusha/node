#include <algorithm>
#include <cassert>
#include <cctype>
#include <cinttypes>
#include <cstdarg>
#include <cstdio>
#include <functional>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>
#include "executable_wrapper.h"
#include "simdutf.h"
#include "uv.h"

#if defined(_WIN32)
#include <io.h>  // _S_IREAD _S_IWRITE
#ifndef S_IRUSR
#define S_IRUSR _S_IREAD
#endif  // S_IRUSR
#ifndef S_IWUSR
#define S_IWUSR _S_IWRITE
#endif  // S_IWUSR
#endif
namespace node {
namespace js2c {
int Main(int argc, char* argv[]);

static bool is_verbose = false;

void Debug(const char* format, ...) {
  va_list arguments;
  va_start(arguments, format);
  if (is_verbose) {
    vfprintf(stderr, format, arguments);
  }
  va_end(arguments);
}

void PrintUvError(const char* syscall, const char* filename, int error) {
  fprintf(stderr, "[%s] %s: %s\n", syscall, filename, uv_strerror(error));
}

int GetStats(const char* path, std::function<void(const uv_stat_t*)> func) {
  uv_fs_t req;
  int r = uv_fs_stat(nullptr, &req, path, nullptr);
  if (r == 0) {
    func(static_cast<const uv_stat_t*>(req.ptr));
  }
  uv_fs_req_cleanup(&req);
  return r;
}

bool IsDirectory(const std::string& filename, int* error) {
  bool result = false;
  *error = GetStats(filename.c_str(), [&](const uv_stat_t* stats) {
    result = !!(stats->st_mode & S_IFDIR);
  });
  if (*error != 0) {
    PrintUvError("stat", filename.c_str(), *error);
  }
  return result;
}

size_t GetFileSize(const std::string& filename, int* error) {
  size_t result = 0;
  *error = GetStats(filename.c_str(),
                    [&](const uv_stat_t* stats) { result = stats->st_size; });
  return result;
}

bool EndsWith(const std::string& str, std::string_view suffix) {
  size_t suffix_len = suffix.length();
  size_t str_len = str.length();
  if (str_len < suffix_len) {
    return false;
  }
  return str.compare(str_len - suffix_len, suffix_len, suffix) == 0;
}

bool StartsWith(const std::string& str, std::string_view prefix) {
  size_t prefix_len = prefix.length();
  size_t str_len = str.length();
  if (str_len < prefix_len) {
    return false;
  }
  return str.compare(0, prefix_len, prefix) == 0;
}

typedef std::vector<std::string> FileList;
typedef std::map<std::string, FileList> FileMap;

bool SearchFiles(const std::string& dir,
                 FileMap* file_map,
                 const std::string& extension) {
  uv_fs_t scan_req;
  int result = uv_fs_scandir(nullptr, &scan_req, dir.c_str(), 0, nullptr);
  bool errored = false;
  if (result < 0) {
    PrintUvError("scandir", dir.c_str(), result);
    errored = true;
  } else {
    auto it = file_map->insert({extension, FileList()}).first;
    FileList& files = it->second;
    files.reserve(files.size() + result);
    uv_dirent_t dent;
    while (true) {
      result = uv_fs_scandir_next(&scan_req, &dent);
      if (result == UV_EOF) {
        break;
      }

      if (result != 0) {
        PrintUvError("scandir_next", dir.c_str(), result);
        errored = true;
        break;
      }

      std::string path = dir + '/' + dent.name;
      if (EndsWith(path, extension)) {
        files.emplace_back(path);
        continue;
      }
      if (!IsDirectory(path, &result)) {
        if (result == 0) {  // It's a file, no need to search further.
          continue;
        } else {
          errored = true;
          break;
        }
      }

      if (!SearchFiles(path, file_map, extension)) {
        errored = true;
        break;
      }
    }
  }

  uv_fs_req_cleanup(&scan_req);
  return !errored;
}

constexpr std::string_view kMjsSuffix = ".mjs";
constexpr std::string_view kJsSuffix = ".js";
constexpr std::string_view kGypiSuffix = ".gypi";
constexpr std::string_view depsPrefix = "deps/";
constexpr std::string_view libPrefix = "lib/";
std::set<std::string_view> kAllowedExtensions{
    kGypiSuffix, kJsSuffix, kMjsSuffix};

std::string_view HasAllowedExtensions(const std::string& filename) {
  for (const auto& ext : kAllowedExtensions) {
    if (EndsWith(filename, ext)) {
      return ext;
    }
  }
  return {};
}

using Fragment = std::vector<char>;
using Fragments = std::vector<std::vector<char>>;

std::vector<char> Join(const Fragments& fragments,
                       const std::string& separator) {
  size_t length = separator.size() * (fragments.size() - 1);
  for (size_t i = 0; i < fragments.size(); ++i) {
    length += fragments[i].size();
  }
  std::vector<char> buf(length, 0);
  size_t cursor = 0;
  for (size_t i = 0; i < fragments.size(); ++i) {
    const Fragment& fragment = fragments[i];
    // Avoid using snprintf on large chunks of data because it's much slower.
    // It's fine to use it on small amount of data though.
    if (i != 0) {
      memcpy(buf.data() + cursor, separator.c_str(), separator.size());
      cursor += separator.size();
    }
    memcpy(buf.data() + cursor, fragment.data(), fragment.size());
    cursor += fragment.size();
  }
  buf.resize(cursor);
  return buf;
}

const char* kTemplate = R"(
#include "env-inl.h"
#include "node_builtins.h"
#include "node_external_reference.h"
#include "node_internals.h"

namespace node {

namespace builtins {

%.*s
namespace {
const ThreadsafeCopyOnWrite<BuiltinSourceMap> global_source_map {
  BuiltinSourceMap {
%.*s
  }  // BuiltinSourceMap
};  // ThreadsafeCopyOnWrite
}  // anonymous namespace

void BuiltinLoader::LoadJavaScriptSource() {
  source_ = global_source_map;
}

void RegisterExternalReferencesForInternalizedBuiltinCode(
  ExternalReferenceRegistry* registry) {
%.*s
}

UnionBytes BuiltinLoader::GetConfig() {
  return UnionBytes(&config_resource);
}

}  // namespace builtins

}  // namespace node
)";

Fragment Format(const Fragments& definitions,
                const Fragments& initializers,
                const Fragments& registrations) {
  std::vector<char> def_buf = Join(definitions, "\n");
  size_t def_size = def_buf.size();
  std::vector<char> init_buf = Join(initializers, "\n");
  size_t init_size = init_buf.size();
  std::vector<char> reg_buf = Join(registrations, "\n");
  size_t reg_size = reg_buf.size();

  size_t result_size =
      def_size + init_size + reg_size + strlen(kTemplate) + 100;
  std::vector<char> result(result_size, 0);
  int r = snprintf(result.data(),
                   result_size,
                   kTemplate,
                   static_cast<int>(def_buf.size()),
                   def_buf.data(),
                   static_cast<int>(init_buf.size()),
                   init_buf.data(),
                   static_cast<int>(reg_buf.size()),
                   reg_buf.data());
  result.resize(r);
  return result;
}

std::vector<char> ReadFileSync(const char* path, size_t size, int* error) {
  uv_fs_t req;
  Debug("ReadFileSync %s with size %zu\n", path, size);

  uv_file file = uv_fs_open(nullptr, &req, path, O_RDONLY, 0, nullptr);
  if (req.result < 0) {
    uv_fs_req_cleanup(&req);
    *error = req.result;
    return std::vector<char>();
  }
  uv_fs_req_cleanup(&req);

  std::vector<char> contents(size);
  size_t offset = 0;

  while (offset < size) {
    uv_buf_t buf = uv_buf_init(contents.data() + offset, size - offset);
    int bytes_read = uv_fs_read(nullptr, &req, file, &buf, 1, offset, nullptr);
    offset += bytes_read;
    *error = req.result;
    uv_fs_req_cleanup(&req);
    if (*error < 0) {
      uv_fs_close(nullptr, &req, file, nullptr);
      // We can't do anything if uv_fs_close returns error, so just return.
      return std::vector<char>();
    }
    if (bytes_read <= 0) {
      break;
    }
  }
  assert(offset == size);

  *error = uv_fs_close(nullptr, &req, file, nullptr);
  return contents;
}

int WriteFileSync(const std::vector<char>& out, const char* path) {
  Debug("WriteFileSync %zu bytes to %s\n", out.size(), path);
  uv_fs_t req;
  uv_file file = uv_fs_open(nullptr,
                            &req,
                            path,
                            UV_FS_O_CREAT | UV_FS_O_WRONLY | UV_FS_O_TRUNC,
                            S_IWUSR | S_IRUSR,
                            nullptr);
  int err = req.result;
  uv_fs_req_cleanup(&req);
  if (err < 0) {
    return err;
  }

  uv_buf_t buf = uv_buf_init(const_cast<char*>(out.data()), out.size());
  err = uv_fs_write(nullptr, &req, file, &buf, 1, 0, nullptr);
  uv_fs_req_cleanup(&req);

  int r = uv_fs_close(nullptr, &req, file, nullptr);
  uv_fs_req_cleanup(&req);
  if (err < 0) {
    // We can't do anything if uv_fs_close returns error, so just return.
    return err;
  }
  return r;
}

int WriteIfChanged(const Fragment& out, const std::string& dest) {
  Debug("output size %zu\n", out.size());

  int error = 0;
  size_t size = GetFileSize(dest, &error);
  if (error != 0 && error != UV_ENOENT) {
    return error;
  }
  Debug("existing size %zu\n", size);

  bool changed = true;
  // If it's not the same size, the file is definitely changed so we'll
  // just proceed to update. Otherwise check the content before deciding
  // whether we want to write it.
  if (error != UV_ENOENT && size == out.size()) {
    std::vector<char> content = ReadFileSync(dest.c_str(), size, &error);
    if (error == 0) {  // In case of error, always write the file.
      changed = (memcmp(content.data(), out.data(), size) != 0);
    }
  }
  if (!changed) {
    Debug("No change, return\n");
    return 0;
  }
  return WriteFileSync(out, dest.c_str());
}

std::string GetFileId(const std::string& fn) {
  std::string filename = fn;
  std::replace(filename.begin(), filename.end(), '\\', '/');
  size_t end = filename.size();
  size_t start = 0;
  std::string prefix;
  // Strip .mjs and .js suffix
  if (EndsWith(filename, kMjsSuffix)) {
    end -= kMjsSuffix.size();
  } else if (EndsWith(filename, kJsSuffix)) {
    end -= kJsSuffix.size();
  }

  // deps/acorn/acorn/dist/acorn.js -> internal/deps/acorn/acorn/dist/acorn
  if (StartsWith(filename, depsPrefix)) {
    start = depsPrefix.size();
    prefix = "internal/deps/";
  } else if (StartsWith(filename, libPrefix)) {
    // lib/internal/url.js -> internal/url
    start = libPrefix.size();
    prefix = "";
  }

  return prefix + std::string(filename.begin() + start, filename.begin() + end);
}

std::string GetVariableName(const std::string& id) {
  std::string result = id;
  size_t length = result.size();

  for (size_t i = 0; i < length; ++i) {
    if (result[i] == '.' || result[i] == '-' || result[i] == '/' || result[i] == '\\') {
      result[i] = '_';
    }
  }
  return result;
}

std::vector<std::string> GetCodeTable() {
  size_t size = 1 << 16;
  std::vector<std::string> code_table(size);
  for (size_t i = 0; i < size; ++i) {
    code_table[i] = std::to_string(i) + ',';
  }
  return code_table;
}

const std::string& GetCode(uint16_t index) {
  static std::vector<std::string> table = GetCodeTable();
  return table[index];
}

#ifdef NODE_JS2C_USE_STRING_LITERALS
const char* string_literal_def_template = "static const %s *%s_raw = ";
constexpr std::string_view ascii_string_literal_start =
    "reinterpret_cast<const uint8_t*>(R\"JS2C1b732aee(";
constexpr std::string_view utf16_string_literal_start =
    "reinterpret_cast<const uint16_t*>(uR\"JS2C1b732aee(";
constexpr std::string_view string_literal_end = ")JS2C1b732aee\");";
#else
const char* array_literal_def_template = "static const %s %s_raw[] = ";
constexpr std::string_view array_literal_start = "{\n";
constexpr std::string_view array_literal_end = "\n};\n\n";
#endif

// Definitions:
// static const uint8_t fs_raw[] = {
//  ....
// };
//
// static StaticExternalOneByteResource fs_resource(fs_raw, 1234, nullptr);
//
// static const uint16_t internal_cli_table_raw[] = {
//  ....
// };
//
// static StaticExternalTwoByteResource
// internal_cli_table_resource(internal_cli_table_raw, 1234, nullptr);
//
// If NODE_JS2C_USE_STRING_LITERALS is defined, the data is output as C++
// raw strings (i.e. R"JS2C1b732aee(...)JS2C1b732aee") rather than as an
// array. This speeds up compilation for gcc/clang.
template <typename T>
Fragment GetDefinitionImpl(const std::vector<char>& code,
                           const std::string& var) {
  constexpr bool is_two_byte = std::is_same_v<T, uint16_t>;
  static_assert(is_two_byte || std::is_same_v<T, char>);

  size_t count = is_two_byte
                     ? simdutf::utf16_length_from_utf8(code.data(), code.size())
                     : code.size();
  constexpr const char* arr_type = is_two_byte ? "uint16_t" : "uint8_t";
  constexpr const char* resource_type = is_two_byte
                                            ? "StaticExternalTwoByteResource"
                                            : "StaticExternalOneByteResource";

#ifdef NODE_JS2C_USE_STRING_LITERALS
  const char* literal_def_template = string_literal_def_template;
  size_t def_size = 512 + code.size();
#else
  const char* literal_def_template = array_literal_def_template;
  constexpr size_t unit =
      (is_two_byte ? 5 : 3) + 1;  // 0-65536 or 0-127 and a ","
  size_t def_size = 512 + count * unit;
#endif

  Fragment result(def_size, 0);

  int cur = snprintf(
      result.data(), def_size, literal_def_template, arr_type, var.c_str());

  assert(cur != 0);

#ifdef NODE_JS2C_USE_STRING_LITERALS
  constexpr std::string_view start_string_view =
      is_two_byte ? utf16_string_literal_start : ascii_string_literal_start;

  memcpy(
      result.data() + cur, start_string_view.data(), start_string_view.size());
  cur += start_string_view.size();

  memcpy(result.data() + cur, code.data(), code.size());
  cur += code.size();

  memcpy(result.data() + cur,
         string_literal_end.data(),
         string_literal_end.size());
  cur += string_literal_end.size();
#else
  memcpy(result.data() + cur,
         array_literal_start.data(),
         array_literal_start.size());
  cur += array_literal_start.size();

  const std::vector<T>* codepoints;

  std::vector<uint16_t> utf16_codepoints;
  if constexpr (is_two_byte) {
    utf16_codepoints.resize(count);
    size_t utf16_count = simdutf::convert_utf8_to_utf16(
        code.data(),
        code.size(),
        reinterpret_cast<char16_t*>(utf16_codepoints.data()));
    assert(utf16_count != 0);
    utf16_codepoints.resize(utf16_count);
    Debug("static size %zu\n", utf16_count);
    codepoints = &utf16_codepoints;
  } else {
    // The code is ASCII, so no need to translate.
    codepoints = &code;
  }

  for (size_t i = 0; i < codepoints->size(); ++i) {
    // Avoid using snprintf on large chunks of data because it's much slower.
    // It's fine to use it on small amount of data though.
    const std::string& str = GetCode(static_cast<uint16_t>((*codepoints)[i]));

    memcpy(result.data() + cur, str.c_str(), str.size());
    cur += str.size();
  }

  memcpy(
      result.data() + cur, array_literal_end.data(), array_literal_end.size());
  cur += array_literal_end.size();
#endif

  int end_size = snprintf(result.data() + cur,
                          result.size() - cur,
                          "static %s %s_resource(%s_raw, %zu, nullptr);\n",
                          resource_type,
                          var.c_str(),
                          var.c_str(),
                          count);
  cur += end_size;
  result.resize(cur);
  return result;
}

Fragment GetDefinition(const std::string& var, const std::vector<char>& code) {
  Debug("GetDefinition %s, code size %zu ", var.c_str(), code.size());
  bool is_one_byte = simdutf::validate_ascii(code.data(), code.size());
  Debug("with %s\n", is_one_byte ? "1-byte chars" : "2-byte chars");

  if (is_one_byte) {
    Debug("static size %zu\n", code.size());
    return GetDefinitionImpl<char>(code, var);
  } else {
    return GetDefinitionImpl<uint16_t>(code, var);
  }
}

int AddModule(const std::string& filename,
              Fragments* definitions,
              Fragments* initializers,
              Fragments* registrations) {
  Debug("AddModule %s start\n", filename.c_str());

  int error = 0;
  size_t file_size = GetFileSize(filename, &error);
  if (error != 0) {
    return error;
  }
  std::vector<char> code = ReadFileSync(filename.c_str(), file_size, &error);
  if (error != 0) {
    return error;
  }
  std::string file_id = GetFileId(filename);
  std::string var = GetVariableName(file_id);

  definitions->emplace_back(GetDefinition(var, code));

  // Initializers of the BuiltinSourceMap:
  // {"fs", UnionBytes{&fs_resource}},
  Fragment& init_buf = initializers->emplace_back(Fragment(256, 0));
  int init_size = snprintf(init_buf.data(),
                           init_buf.size(),
                           "    {\"%s\", UnionBytes(&%s_resource) },",
                           file_id.c_str(),
                           var.c_str());
  init_buf.resize(init_size);

  // Registrations:
  // registry->Register(&fs_resource);
  Fragment& reg_buf = registrations->emplace_back(Fragment(256, 0));
  int reg_size = snprintf(reg_buf.data(),
                          reg_buf.size(),
                          "  registry->Register(&%s_resource);",
                          var.c_str());
  reg_buf.resize(reg_size);
  return 0;
}

std::vector<char> ReplaceAll(const std::vector<char>& data,
                             const std::string& search,
                             const std::string& replacement) {
  auto cur = data.begin();
  auto last = data.begin();
  std::vector<char> result;
  result.reserve(data.size());
  while ((cur = std::search(last, data.end(), search.begin(), search.end())) !=
         data.end()) {
    result.insert(result.end(), last, cur);
    result.insert(result.end(),
                  replacement.c_str(),
                  replacement.c_str() + replacement.size());
    last = cur + search.size();
  }
  result.insert(result.end(), last, data.end());
  return result;
}

std::vector<char> StripComments(const std::vector<char>& input) {
  std::vector<char> result;
  result.reserve(input.size());

  auto last_hash = input.cbegin();
  auto line_begin = input.cbegin();
  auto end = input.cend();
  while ((last_hash = std::find(line_begin, end, '#')) != end) {
    result.insert(result.end(), line_begin, last_hash);
    line_begin = std::find(last_hash, end, '\n');
    if (line_begin != end) {
      line_begin += 1;
    }
  }
  result.insert(result.end(), line_begin, end);
  return result;
}

// This is technically unused for our config.gypi, but just porting it here to
// mimic js2c.py.
std::vector<char> JoinMultilineString(const std::vector<char>& input) {
  std::vector<char> result;
  result.reserve(input.size());

  auto closing_quote = input.cbegin();
  auto last_inserted = input.cbegin();
  auto end = input.cend();
  std::string search = "'\n";
  while ((closing_quote = std::search(
              last_inserted, end, search.begin(), search.end())) != end) {
    if (closing_quote != last_inserted) {
      result.insert(result.end(), last_inserted, closing_quote - 1);
      last_inserted = closing_quote - 1;
    }
    auto opening_quote = closing_quote + 2;
    while (opening_quote != end && isspace(*opening_quote)) {
      opening_quote++;
    }
    if (opening_quote == end) {
      break;
    }
    if (*opening_quote == '\'') {
      last_inserted = opening_quote + 1;
    } else {
      result.insert(result.end(), last_inserted, opening_quote);
      last_inserted = opening_quote;
    }
  }
  result.insert(result.end(), last_inserted, end);
  return result;
}

std::vector<char> JSONify(const std::vector<char>& code) {
  // 1. Remove string comments
  std::vector<char> stripped = StripComments(code);

  // 2. join multiline strings
  std::vector<char> joined = JoinMultilineString(stripped);

  // 3. normalize string literals from ' into "
  for (size_t i = 0; i < joined.size(); ++i) {
    if (joined[i] == '\'') {
      joined[i] = '"';
    }
  }

  // 4. turn pseudo-booleans strings into Booleans
  std::vector<char> result3 = ReplaceAll(joined, R"("true")", "true");
  std::vector<char> result4 = ReplaceAll(result3, R"("false")", "false");

  return result4;
}

int AddGypi(const std::string& var,
            const std::string& filename,
            Fragments* definitions) {
  Debug("AddGypi %s start\n", filename.c_str());

  int error = 0;
  size_t file_size = GetFileSize(filename, &error);
  if (error != 0) {
    return error;
  }
  std::vector<char> code = ReadFileSync(filename.c_str(), file_size, &error);
  if (error != 0) {
    return error;
  }
  assert(var == "config");

  std::vector<char> transformed = JSONify(code);
  definitions->emplace_back(GetDefinition(var, transformed));
  return 0;
}

int JS2C(const FileList& js_files,
         const FileList& mjs_files,
         const std::string& config,
         const std::string& dest) {
  Fragments definitions;
  definitions.reserve(js_files.size() + mjs_files.size() + 1);
  Fragments initializers;
  initializers.reserve(js_files.size() + mjs_files.size());
  Fragments registrations;
  registrations.reserve(js_files.size() + mjs_files.size() + 1);

  for (const auto& filename : js_files) {
    int r = AddModule(filename, &definitions, &initializers, &registrations);
    if (r != 0) {
      return r;
    }
  }
  for (const auto& filename : mjs_files) {
    int r = AddModule(filename, &definitions, &initializers, &registrations);
    if (r != 0) {
      return r;
    }
  }

  assert(config == "config.gypi");
  // "config.gypi" -> config_raw.
  int r = AddGypi("config", config, &definitions);
  if (r != 0) {
    return r;
  }
  Fragment out = Format(definitions, initializers, registrations);
  return WriteIfChanged(out, dest);
}

int PrintUsage(const char* argv0) {
  fprintf(stderr,
          "Usage: %s [--verbose] [--root /path/to/project/root] "
          "path/to/output.cc path/to/directory "
          "[extra-files ...]\n",
          argv0);
  return 1;
}

int Main(int argc, char* argv[]) {
  if (argc < 3) {
    return PrintUsage(argv[0]);
  }

  std::vector<std::string> args;
  args.reserve(argc);
  std::string root_dir;
  for (int i = 1; i < argc; ++i) {
    std::string arg(argv[i]);
    if (arg == "--verbose") {
      is_verbose = true;
    } else if (arg == "--root") {
      if (i == argc - 1) {
        fprintf(stderr, "--root must be followed by a path\n");
        return 1;
      }
      root_dir = argv[++i];
    } else {
      args.emplace_back(argv[i]);
    }
  }

  if (args.size() < 2) {
    return PrintUsage(argv[0]);
  }

  if (!root_dir.empty()) {
    int r = uv_chdir(root_dir.c_str());
    if (r != 0) {
      fprintf(stderr, "Cannot switch to the directory specified by --root\n");
      PrintUvError("chdir", root_dir.c_str(), r);
      return 1;
    }
  }
  std::string output = args[0];

  FileMap file_map;
  for (size_t i = 1; i < args.size(); ++i) {
    int error = 0;
    const std::string& file = args[i];
    if (IsDirectory(file, &error)) {
      if (!SearchFiles(file, &file_map, std::string(kJsSuffix)) ||
          !SearchFiles(file, &file_map, std::string(kMjsSuffix))) {
        return 1;
      }
    } else if (error != 0) {
      return 1;
    } else {  // It's a file.
      std::string_view extension = HasAllowedExtensions(file);
      if (extension.size() != 0) {
        auto it = file_map.insert({std::string(extension), FileList()}).first;
        it->second.push_back(file);
      } else {
        fprintf(stderr, "Unsupported file: %s\n", file.c_str());
        return 1;
      }
    }
  }

  // Should have exactly 3 types: `.js`, `.mjs` and `.gypi`.
  assert(file_map.size() == 3);
  auto gypi_it = file_map.find(".gypi");
  std::string config = "config.gypi";
  // Currently config.gypi is the only `.gypi` file allowed
  if (gypi_it == file_map.end() || gypi_it->second.size() != 1 ||
      gypi_it->second[0] != config) {
    fprintf(
        stderr,
        "Arguments should contain one and only one .gypi file: config.gypi\n");
    return 1;
  }
  auto js_it = file_map.find(".js");
  auto mjs_it = file_map.find(".mjs");
  assert(js_it != file_map.end() && mjs_it != file_map.end());

  std::sort(js_it->second.begin(), js_it->second.end());
  std::sort(mjs_it->second.begin(), mjs_it->second.end());

  return JS2C(js_it->second, mjs_it->second, config, output);
}
}  // namespace js2c
}  // namespace node

NODE_MAIN(int argc, node::argv_type raw_argv[]) {
  char** argv;
  node::FixupMain(argc, raw_argv, &argv);
  return node::js2c::Main(argc, argv);
}
