import std;

#include "harness.hh"
#include "llvm_fixture_normalize.hh"

TEST_CASE("llvm intrinsic printer attributes are version tolerant")
{
    auto old = "; Function Attrs: nounwind\ndeclare void @llvm.memcpy() #0\nattributes #0 = { nounwind }\n";
    auto newer = "; Function Attrs: nosync nounwind\ndeclare void @llvm.memcpy() #0\nattributes #0 = { nosync nounwind }\n";
    CHECK_EQ(normalize_llvm_printer_versions(old), normalize_llvm_printer_versions(newer));
    auto own = "; Function Attrs: nosync nounwind\ndefine void @dcc() #0\nattributes #0 = { nosync nounwind }\n";
    CHECK_EQ(normalize_llvm_printer_versions(own), std::string(own));
    auto shared = "declare void @llvm.memcpy() #0\ndefine void @dcc() #0\nattributes #0 = { nosync nounwind }\n";
    CHECK_EQ(normalize_llvm_printer_versions(shared), std::string(shared));
    CHECK(normalize_llvm_printer_versions(old) != normalize_llvm_printer_versions("declare i32 @llvm.memcpy() #0\nattributes #0 = { nounwind }\n"));
    CHECK(normalize_llvm_printer_versions(old) != normalize_llvm_printer_versions("; Function Attrs: noreturn\ndeclare void @llvm.memcpy() #0\nattributes #0 = { noreturn }\n"));
}

TEST_CASE("llvm module asm printer layout preserves ordered contents")
{
    auto old = "module asm \".quad first\"\nmodule asm \".quad second\"\n";
    auto newer = "module asm\n    \".quad first\"\n    \".quad second\"\n";
    CHECK_EQ(normalize_llvm_printer_versions(old), normalize_llvm_printer_versions(newer));
    CHECK(normalize_llvm_printer_versions(old) != normalize_llvm_printer_versions("module asm \".quad second\"\nmodule asm \".quad first\"\n"));
}
