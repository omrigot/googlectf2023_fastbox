// Copyright 2020 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "sandboxed_api/tools/clang_generator/emitter.h"

#include "absl/random/random.h"
#include "absl/status/statusor.h"
#include "absl/strings/ascii.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/strings/str_join.h"
#include "absl/strings/str_replace.h"
#include "absl/strings/str_split.h"
#include "absl/strings/string_view.h"
#include "absl/strings/strip.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/DeclTemplate.h"
#include "clang/AST/QualTypeNames.h"
#include "clang/AST/Type.h"
#include "clang/Format/Format.h"
#include "sandboxed_api/tools/clang_generator/diagnostics.h"
#include "sandboxed_api/tools/clang_generator/generator.h"
#include "sandboxed_api/tools/clang_generator/types.h"
#include "sandboxed_api/util/status_macros.h"

namespace sapi {

// Common file prolog with auto-generation notice.
// Note: The includes will be adjusted by Copybara when converting to/from
//       internal code. This is intentional.
// Text template arguments:
//   1. Header guard
constexpr absl::string_view kHeaderProlog =
    R"(// AUTO-GENERATED by the Sandboxed API generator.
// Edits will be discarded when regenerating this file.

#ifndef %1$s
#define %1$s

#include <cstdint>
#include <type_traits>

#include "absl/base/macros.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "sandboxed_api/sandbox.h"
#include "sandboxed_api/util/status_macros.h"
#include "sandboxed_api/vars.h"

)";
constexpr absl::string_view kHeaderEpilog =
    R"(
#endif  // %1$s)";

// Text template arguments:
//   1. Include for embedded sandboxee objects
constexpr absl::string_view kEmbedInclude = R"(#include "%1$s_embed.h"

)";

// Text template arguments:
//   1. Namespace name
constexpr absl::string_view kNamespaceBeginTemplate =
    R"(
namespace %1$s {

)";
constexpr absl::string_view kNamespaceEndTemplate =
    R"(
}  // namespace %1$s
)";

// Text template arguments:
//   1. Class name
//   2. Embedded object identifier
constexpr absl::string_view kEmbedClassTemplate = R"(
// Sandbox with embedded sandboxee and default policy
class %1$s : public ::sapi::Sandbox {
 public:
  %1$s() : ::sapi::Sandbox(%2$s_embed_create()) {}
};

)";

// Text template arguments:
//   1. Class name
constexpr absl::string_view kClassHeaderTemplate = R"(
// Sandboxed API
class %1$s {
 public:
  explicit %1$s(::sapi::Sandbox* sandbox) : sandbox_(sandbox) {}

  ABSL_DEPRECATED("Call sandbox() instead")
  ::sapi::Sandbox* GetSandbox() const { return sandbox(); }
  ::sapi::Sandbox* sandbox() const { return sandbox_; }
)";

constexpr absl::string_view kClassFooterTemplate = R"(
 private:
  ::sapi::Sandbox* sandbox_;
};
)";

namespace internal {

absl::StatusOr<std::string> ReformatGoogleStyle(const std::string& filename,
                                                const std::string& code,
                                                int column_limit) {
  // Configure code style based on Google style, but enforce pointer alignment
  clang::format::FormatStyle style =
      clang::format::getGoogleStyle(clang::format::FormatStyle::LK_Cpp);
  style.DerivePointerAlignment = false;
  style.PointerAlignment = clang::format::FormatStyle::PAS_Left;
  if (column_limit >= 0) {
    style.ColumnLimit = column_limit;
  }

  clang::tooling::Replacements replacements = clang::format::reformat(
      style, code, llvm::ArrayRef(clang::tooling::Range(0, code.size())),
      filename);

  llvm::Expected<std::string> formatted_header =
      clang::tooling::applyAllReplacements(code, replacements);
  if (!formatted_header) {
    return absl::InternalError(llvm::toString(formatted_header.takeError()));
  }
  return *formatted_header;
}

}  // namespace internal

std::string GetIncludeGuard(absl::string_view filename) {
  if (filename.empty()) {
    static auto* bit_gen = new absl::BitGen();
    return absl::StrCat(
        // Copybara will transform the string. This is intentional.
        "SANDBOXED_API_GENERATED_HEADER_",
        absl::AsciiStrToUpper(absl::StrCat(
            absl::Hex(absl::Uniform<uint64_t>(*bit_gen), absl::kZeroPad16))),
        "_");
  }

  constexpr absl::string_view kUnderscorePrefix = "SAPI_";
  std::string guard;
  guard.reserve(filename.size() + kUnderscorePrefix.size() + 1);
  for (auto c : filename) {
    if (absl::ascii_isalpha(c)) {
      guard += absl::ascii_toupper(c);
      continue;
    }
    if (guard.empty()) {
      guard = kUnderscorePrefix;
    }
    if (absl::ascii_isdigit(c)) {
      guard += c;
    } else if (guard.back() != '_') {
      guard += '_';
    }
  }
  if (!absl::EndsWith(guard, "_")) {
    guard += '_';
  }
  return guard;
}

// Returns the namespace components of a declaration's qualified name.
std::vector<std::string> GetNamespacePath(const clang::TypeDecl* decl) {
  std::vector<std::string> comps;
  for (const auto* ctx = decl->getDeclContext(); ctx; ctx = ctx->getParent()) {
    if (const auto* nd = llvm::dyn_cast<clang::NamespaceDecl>(ctx)) {
      comps.push_back(nd->getName().str());
    }
  }
  std::reverse(comps.begin(), comps.end());
  return comps;
}

std::string PrintRecordTemplateArguments(const clang::CXXRecordDecl* record) {
  const auto* template_inst_decl = record->getTemplateInstantiationPattern();
  if (!template_inst_decl) {
    return "";
  }
  const auto* template_decl = template_inst_decl->getDescribedClassTemplate();
  if (!template_decl) {
    return "";
  }
  const auto* template_params = template_decl->getTemplateParameters();
  if (!template_params) {
    return "";
  }
  clang::ASTContext& context = record->getASTContext();
  std::vector<std::string> params;
  params.reserve(template_params->size());
  for (const auto& template_param : *template_params) {
    if (const auto* p =
            llvm::dyn_cast<clang::NonTypeTemplateParmDecl>(template_param)) {
      // TODO(cblichmann): Should be included by CollectRelatedTypes().
      params.push_back(clang::TypeName::getFullyQualifiedName(
          p->getType().getDesugaredType(context), context,
          context.getPrintingPolicy()));
    } else {  // Also covers template template parameters
      params.push_back("typename");
    }
    absl::StrAppend(&params.back(), " /*",
                    std::string(template_param->getName()), "*/");
  }
  return absl::StrCat("template <", absl::StrJoin(params, ", "), ">");
}

// Serializes the given Clang AST declaration back into compilable source code.
std::string PrintDecl(const clang::Decl* decl) {
  std::string pretty;
  llvm::raw_string_ostream os(pretty);
  decl->print(os);
  return os.str();
}

// Returns the spelling for a given declaration will be emitted to the final
// header. This may rewrite declarations (like converting typedefs to using,
// etc.). Note that the resulting spelling will need to be wrapped inside a
// namespace if the original declaration was inside one.
std::string GetSpelling(const clang::Decl* decl) {
  // TODO(cblichmann): Make types nicer
  //   - Rewrite typedef to using
  //   - Rewrite function pointers using std::add_pointer_t<>;

  if (const auto* typedef_decl = llvm::dyn_cast<clang::TypedefNameDecl>(decl)) {
    // Special case: anonymous enum/struct
    if (auto* tag_decl = typedef_decl->getAnonDeclWithTypedefName()) {
      return absl::StrCat("typedef ", PrintDecl(tag_decl), " ",
                          ToStringView(typedef_decl->getName()));
    }
  }

  if (const auto* record_decl = llvm::dyn_cast<clang::CXXRecordDecl>(decl)) {
    if (!record_decl->isCLike()) {
      // For C++ classes/structs, only emit a forward declaration.
      return absl::StrCat(PrintRecordTemplateArguments(record_decl),
                          record_decl->isClass() ? "class " : "struct ",
                          ToStringView(record_decl->getName()));
    }
  }
  return PrintDecl(decl);
}

std::string GetParamName(const clang::ParmVarDecl* decl, int index) {
  if (std::string name = decl->getName().str(); !name.empty()) {
    return absl::StrCat(name, "_");  // Suffix to avoid collisions
  }
  return absl::StrCat("unnamed", index, "_");
}

absl::StatusOr<std::string> PrintFunctionPrototypeComment(
    const clang::FunctionDecl* decl) {
  const clang::ASTContext& context = decl->getASTContext();

  std::string out = absl::StrCat(
      MapQualTypeParameterForCxx(context, decl->getDeclaredReturnType()), " ",
      decl->getQualifiedNameAsString(), "(");

  std::string print_separator;
  for (int i = 0; i < decl->getNumParams(); ++i) {
    const clang::ParmVarDecl* param = decl->getParamDecl(i);

    absl::StrAppend(&out, print_separator);
    print_separator = ", ";
    absl::StrAppend(&out,
                    MapQualTypeParameterForCxx(context, param->getType()));
    if (std::string name = param->getName().str(); !name.empty()) {
      absl::StrAppend(&out, " ", name);
    }
  }
  absl::StrAppend(&out, ")");

  SAPI_ASSIGN_OR_RETURN(
      std::string formatted,
      internal::ReformatGoogleStyle(/*filename=*/"input", out, 75));
  out.clear();
  for (const auto& line : absl::StrSplit(formatted, '\n')) {
    absl::StrAppend(&out, "// ", line, "\n");
  }
  return out;
}

absl::StatusOr<std::string> EmitFunction(const clang::FunctionDecl* decl) {
  const clang::QualType return_type = decl->getDeclaredReturnType();
  if (return_type->isRecordType()) {
    return MakeStatusWithDiagnostic(
        decl->getBeginLoc(), absl::StatusCode::kCancelled,
        "returning record by value, skipping function");
  }
  std::string out;

  SAPI_ASSIGN_OR_RETURN(std::string prototype,
                        PrintFunctionPrototypeComment(decl));
  absl::StrAppend(&out, "\n", prototype);

  auto function_name = ToStringView(decl->getName());
  const bool returns_void = return_type->isVoidType();

  const clang::ASTContext& context = decl->getASTContext();

  // "Status<OptionalReturn> FunctionName("
  absl::StrAppend(&out, MapQualTypeReturn(context, return_type), " ",
                  function_name, "(");

  struct ParameterInfo {
    clang::QualType qual;
    std::string name;
  };
  std::vector<ParameterInfo> params;

  std::string print_separator;
  for (int i = 0; i < decl->getNumParams(); ++i) {
    const clang::ParmVarDecl* param = decl->getParamDecl(i);
    if (param->getType()->isRecordType()) {
      return MakeStatusWithDiagnostic(
          param->getBeginLoc(), absl::StatusCode::kCancelled,
          absl::StrCat("passing record parameter '",
                       ToStringView(param->getName()),
                       "' by value, skipping function"));
    }

    ParameterInfo& param_info = params.emplace_back();
    param_info.qual = param->getType();
    param_info.name = GetParamName(param, i);

    absl::StrAppend(&out, print_separator);
    print_separator = ", ";
    absl::StrAppend(&out, MapQualTypeParameter(context, param_info.qual), " ",
                    param_info.name);
  }

  absl::StrAppend(&out, ") {\n");
  absl::StrAppend(&out, MapQualType(context, return_type), " v_ret_;\n");
  for (const auto& [qual, name] : params) {
    if (!IsPointerOrReference(qual)) {
      absl::StrAppend(&out, MapQualType(context, qual), " v_", name, "(", name,
                      ");\n");
    }
  }
  absl::StrAppend(&out, "\nSAPI_RETURN_IF_ERROR(sandbox_->Call(\"",
                  function_name, "\", &v_ret_");
  for (const auto& [qual, name] : params) {
    absl::StrAppend(&out, ", ", IsPointerOrReference(qual) ? "" : "&v_", name);
  }
  absl::StrAppend(&out, "));\nreturn ",
                  (returns_void ? "::absl::OkStatus()" : "v_ret_.GetValue()"),
                  ";\n}\n");
  return out;
}

absl::StatusOr<std::string> EmitHeader(
    const std::vector<std::string>& function_definitions,
    const std::vector<const RenderedType*>& rendered_types,
    const GeneratorOptions& options) {
  std::string out;
  const std::string include_guard = GetIncludeGuard(options.out_file);
  absl::StrAppendFormat(&out, kHeaderProlog, include_guard);
  // When embedding the sandboxee, add embed header include
  if (!options.embed_name.empty()) {
    // Not using JoinPath() because even on Windows include paths use plain
    // slashes.
    std::string include_file(absl::StripSuffix(
        absl::StrReplaceAll(options.embed_dir, {{"\\", "/"}}), "/"));
    if (!include_file.empty()) {
      absl::StrAppend(&include_file, "/");
    }
    absl::StrAppend(&include_file, options.embed_name);
    absl::StrAppendFormat(&out, kEmbedInclude, include_file);
  }

  // If specified, wrap the generated API in a namespace
  if (options.has_namespace()) {
    absl::StrAppendFormat(&out, kNamespaceBeginTemplate,
                          options.namespace_name);
  }

  // Emit type dependencies
  if (!rendered_types.empty()) {
    absl::StrAppend(&out, "// Types this API depends on\n");
    std::string last_ns_name;
    for (const RenderedType* rt : rendered_types) {
      const auto& [ns_name, spelling] = *rt;
      if (last_ns_name != ns_name) {
        if (!last_ns_name.empty()) {
          absl::StrAppend(&out, "}  // namespace ", last_ns_name, "\n\n");
        }
        if (!ns_name.empty()) {
          absl::StrAppend(&out, "namespace ", ns_name, " {\n");
        }
        last_ns_name = ns_name;
      }

      absl::StrAppend(&out, spelling, ";\n");
    }
    if (!last_ns_name.empty()) {
      absl::StrAppend(&out, "}  // namespace ", last_ns_name, "\n\n");
    }
  }

  // Optionally emit a default sandbox that instantiates an embedded sandboxee
  if (!options.embed_name.empty()) {
    // TODO(cblichmann): Make the "Sandbox" suffix configurable.
    absl::StrAppendFormat(
        &out, kEmbedClassTemplate, absl::StrCat(options.name, "Sandbox"),
        absl::StrReplaceAll(options.embed_name, {{"-", "_"}}));
  }

  // Emit the actual Sandboxed API
  // TODO(cblichmann): Make the "Api" suffix configurable or at least optional.
  absl::StrAppendFormat(&out, kClassHeaderTemplate,
                        absl::StrCat(options.name, "Api"));
  absl::StrAppend(&out, absl::StrJoin(function_definitions, "\n"));
  absl::StrAppend(&out, kClassFooterTemplate);

  // Close out the header: close namespace (if needed) and end include guard
  if (options.has_namespace()) {
    absl::StrAppendFormat(&out, kNamespaceEndTemplate, options.namespace_name);
  }
  absl::StrAppendFormat(&out, kHeaderEpilog, include_guard);
  return out;
}

void Emitter::EmitType(clang::TypeDecl* type_decl) {
  if (!type_decl) {
    return;
  }

  // Skip types defined in system headers.
  // TODO(cblichmann): Instead of this and the hard-coded entities below, we
  //                   should map types and add the correct (system) headers to
  //                   the generated output.
  if (type_decl->getASTContext().getSourceManager().isInSystemHeader(
          type_decl->getBeginLoc())) {
    return;
  }

  const std::vector<std::string> ns_path = GetNamespacePath(type_decl);
  std::string ns_name;
  if (!ns_path.empty()) {
    const auto& ns_root = ns_path.front();
    // Filter out declarations from the C++ standard library, from SAPI itself
    // and from other well-known namespaces.
    if (ns_root == "std" || ns_root == "__gnu_cxx" || ns_root == "sapi") {
      return;
    }
    if (ns_root == "absl") {
      // Skip Abseil internal namespaces
      if (ns_path.size() > 1 && absl::EndsWith(ns_path[1], "_internal")) {
        return;
      }
      // Skip types from Abseil that will already be included in the generated
      // header.
      if (auto name = ToStringView(type_decl->getName());
          name == "CordMemoryAccounting" || name == "Duration" ||
          name == "LogEntry" || name == "LogSeverity" || name == "Span" ||
          name == "StatusCode" || name == "StatusToStringMode" ||
          name == "SynchLocksHeld" || name == "SynchWaitParams" ||
          name == "Time" || name == "string_view" || name == "tid_t") {
        return;
      }
    }
    ns_name = absl::StrJoin(ns_path, "::");
  }

  std::string spelling = GetSpelling(type_decl);
  if (const auto& [it, inserted] = rendered_types_.emplace(ns_name, spelling);
      inserted) {
    rendered_types_ordered_.push_back(&*it);
  }
}

void Emitter::AddTypeDeclarations(
    const std::vector<clang::TypeDecl*>& type_decls) {
  for (clang::TypeDecl* type_decl : type_decls) {
    EmitType(type_decl);
  }
}

absl::Status Emitter::AddFunction(clang::FunctionDecl* decl) {
  if (rendered_functions_.insert(decl->getQualifiedNameAsString()).second) {
    SAPI_ASSIGN_OR_RETURN(std::string function, EmitFunction(decl));
    rendered_functions_ordered_.push_back(function);
  }
  return absl::OkStatus();
}

absl::StatusOr<std::string> Emitter::EmitHeader(
    const GeneratorOptions& options) {
  SAPI_ASSIGN_OR_RETURN(const std::string header,
                        ::sapi::EmitHeader(rendered_functions_ordered_,
                                           rendered_types_ordered_, options));
  return internal::ReformatGoogleStyle(options.out_file, header);
}

}  // namespace sapi