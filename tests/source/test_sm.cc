import std;
import dcc.sm;
import dcc.utf8;

#include "harness.hh"

namespace sm = dcc::sm;

SECTION("sm: lsp_position bounds checking");

TEST_CASE("lsp_position with offset past end returns OutOfRange")
{
    sm::SourceManager mgr;
    auto fid = mgr.open_in_memory("file:///test.dc", "hello world", 1);
    CHECK(fid != sm::FileId::Invalid);

    auto const* sf = mgr.get(fid);
    REQUIRE(sf != nullptr);
    CHECK_EQ(sf->size(), 11u);

    auto pos = sf->lsp_position(12);
    CHECK(!pos.has_value());
    CHECK_EQ(pos.error(), sm::Error::OutOfRange);

    pos = sf->lsp_position(9999);
    CHECK(!pos.has_value());
    CHECK_EQ(pos.error(), sm::Error::OutOfRange);
}

TEST_CASE("lsp_position with offset at size returns end position")
{
    sm::SourceManager mgr;
    auto fid = mgr.open_in_memory("file:///test.dc", "abc", 1);
    REQUIRE(fid != sm::FileId::Invalid);

    auto const* sf = mgr.get(fid);
    REQUIRE(sf != nullptr);
    CHECK_EQ(sf->size(), 3u);

    auto pos = sf->lsp_position(3);
    REQUIRE(pos.has_value());
    CHECK_EQ(pos->line, 0u);
    CHECK_EQ(pos->character, 3u);
}

TEST_CASE("lsp_position with valid offset returns Position")
{
    sm::SourceManager mgr;
    auto fid = mgr.open_in_memory("file:///test.dc", "hello\nworld", 1);
    REQUIRE(fid != sm::FileId::Invalid);

    auto const* sf = mgr.get(fid);
    REQUIRE(sf != nullptr);

    auto pos = sf->lsp_position(0);
    REQUIRE(pos.has_value());
    CHECK_EQ(pos->line, 0u);
    CHECK_EQ(pos->character, 0u);

    pos = sf->lsp_position(6);
    REQUIRE(pos.has_value());
    CHECK_EQ(pos->line, 1u);
    CHECK_EQ(pos->character, 0u);

    pos = sf->lsp_position(6);
    REQUIRE(pos.has_value());
    CHECK_EQ(pos->line, 1u);
    CHECK_EQ(pos->character, 0u);
}

TEST_CASE("lsp_position on empty file returns Position(0,0) for offset 0")
{
    sm::SourceManager mgr;
    auto fid = mgr.open_in_memory("file:///test.dc", "", 1);
    REQUIRE(fid != sm::FileId::Invalid);

    auto const* sf = mgr.get(fid);
    REQUIRE(sf != nullptr);
    CHECK_EQ(sf->size(), 0u);

    auto pos = sf->lsp_position(0);
    REQUIRE(pos.has_value());
    CHECK_EQ(pos->line, 0u);
    CHECK_EQ(pos->character, 0u);
}

TEST_CASE("offset_at_lsp_position with out-of-bounds line returns OutOfRange")
{
    sm::SourceManager mgr;
    auto fid = mgr.open_in_memory("file:///test.dc", "hello\nworld", 1);
    REQUIRE(fid != sm::FileId::Invalid);

    auto loc = mgr.lsp_position_to_location(fid, 100u, 0u);
    CHECK(!loc.has_value());
    CHECK_EQ(loc.error(), sm::Error::OutOfRange);
}

SECTION("sm: location_to_lsp_position robustness");

TEST_CASE("location_to_lsp_position on invalid FileId returns OutOfRange")
{
    sm::SourceManager mgr;
    sm::Location bad_loc{sm::FileId::Invalid, 0};
    auto pos = mgr.location_to_lsp_position(bad_loc);
    CHECK(!pos.has_value());
    CHECK_EQ(pos.error(), sm::Error::OutOfRange);
}

TEST_CASE("location_to_lsp_position on nonexistent FileId returns OutOfRange")
{
    sm::SourceManager mgr;
    sm::Location bad_loc{static_cast<sm::FileId>(999), 0};
    auto pos = mgr.location_to_lsp_position(bad_loc);
    CHECK(!pos.has_value());
    CHECK_EQ(pos.error(), sm::Error::OutOfRange);
}

SECTION("sm: semantic token cross-file range filtering");

TEST_CASE("SourceRanges from different files are distinguishable")
{
    sm::SourceManager mgr;
    auto fid_a = mgr.open_in_memory("file:///a.dc", "hello", 1);
    auto fid_b = mgr.open_in_memory("file:///b.dc", "world", 1);
    REQUIRE(fid_a != sm::FileId::Invalid);
    REQUIRE(fid_b != sm::FileId::Invalid);
    CHECK_NE(fid_a, fid_b);

    sm::SourceRange range_a{sm::Location{fid_a, 0}, sm::Location{fid_a, 1}};
    CHECK(range_a.valid());

    sm::SourceRange range_b{sm::Location{fid_b, 0}, sm::Location{fid_b, 1}};
    CHECK(range_b.valid());

    sm::FileId const requested = fid_a;
    CHECK_EQ(range_a.begin.fileId, requested);
    CHECK_EQ(range_a.end.fileId, requested);
    CHECK_NE(range_b.begin.fileId, requested);
    CHECK_NE(range_b.end.fileId, requested);
}

TEST_CASE("SourceRanges with invalid file ids are not valid")
{
    sm::SourceRange invalid_range{sm::Location{sm::FileId::Invalid, 0}, sm::Location{sm::FileId::Invalid, 1}};
    CHECK(!invalid_range.valid());

    sm::SourceRange mixed_range{sm::Location{sm::FileId::Invalid, 0}, sm::Location{static_cast<sm::FileId>(1), 1}};
    CHECK(!mixed_range.valid());
}

TEST_CASE("SourceRange with begin > end is not valid")
{
    sm::SourceManager mgr;
    auto fid = mgr.open_in_memory("file:///t.dc", "abcdef", 1);
    REQUIRE(fid != sm::FileId::Invalid);

    sm::SourceRange reversed{sm::Location{fid, 3}, sm::Location{fid, 1}};
    CHECK(!reversed.valid());
}

TEST_CASE("location_to_lsp_position returns OutOfRange for offset past end")
{
    sm::SourceManager mgr;
    auto fid = mgr.open_in_memory("file:///t.dc", "hi", 1);
    REQUIRE(fid != sm::FileId::Invalid);

    sm::Location bad{fid, 99};
    auto pos = mgr.location_to_lsp_position(bad);
    CHECK(!pos.has_value());
    CHECK_EQ(pos.error(), sm::Error::OutOfRange);

    sm::Location ok{fid, 1};
    pos = mgr.location_to_lsp_position(ok);
    REQUIRE(pos.has_value());
}

SECTION("sm: update_in_memory failure handling");

TEST_CASE("update_in_memory on nonexistent URI returns FileNotFound")
{
    sm::SourceManager mgr;
    auto result = mgr.update_in_memory("file:///nonexistent.dc", "content", 1);
    CHECK(!result.has_value());
    CHECK_EQ(result.error(), sm::Error::FileNotFound);
}

TEST_CASE("update_in_memory on disk-loaded file returns PermissionDenied")
{
    auto temp_file = std::filesystem::temp_directory_path() / "dcc_test_permission.tmp";

    std::error_code ec;
    std::filesystem::remove(temp_file, ec);

    {
        std::ofstream ofs(temp_file);
        REQUIRE(ofs.is_open());
        ofs << "module test;";
    }
    CHECK(std::filesystem::file_size(temp_file) > 0);

    sm::SourceManager mgr;
    auto load_result = mgr.load(temp_file);
    REQUIRE(load_result.has_value());
    sm::FileId fid = *load_result;

    auto const* sf = mgr.get(fid);
    REQUIRE(sf != nullptr);
    CHECK_EQ(sf->kind(), sm::FileKind::Disk);

    auto uri = sf->uri();
    CHECK(!uri.empty());

    auto result = mgr.update_in_memory(uri, "new content", 2);
    CHECK(!result.has_value());
    CHECK_EQ(result.error(), sm::Error::PermissionDenied);

    std::filesystem::remove(temp_file, ec);
}

TEST_CASE("open_in_memory and update_in_memory round-trip preserves content")
{
    sm::SourceManager mgr;
    auto fid = mgr.open_in_memory("file:///test.dc", "original", 1);
    REQUIRE(fid != sm::FileId::Invalid);

    auto const* sf = mgr.get(fid);
    REQUIRE(sf != nullptr);
    CHECK_EQ(sf->text(), "original");

    auto result = mgr.update_in_memory("file:///test.dc", "modified", 2);
    CHECK(result.has_value());

    CHECK_EQ(sf->text(), "modified");
    CHECK_EQ(sf->version(), 2);
}

TEST_CASE("update_in_memory rejects a closed document and open_in_memory reopens cleanly")
{
    sm::SourceManager mgr;
    auto fid = mgr.open_in_memory("file:///test.dc", "initial", 1);
    REQUIRE(fid != sm::FileId::Invalid);

    auto close_result = mgr.close_in_memory("file:///test.dc");
    CHECK(close_result.has_value());

    auto result = mgr.update_in_memory("file:///test.dc", "after-close", 2);
    CHECK(!result.has_value());
    CHECK_EQ(result.error(), sm::Error::NotOpen);

    auto const* sf = mgr.get(fid);
    REQUIRE(sf != nullptr);
    CHECK(sf->is_closed());
    CHECK_EQ(sf->text(), "initial");
    CHECK_EQ(sf->version(), 1);

    auto reopened = mgr.open_in_memory("file:///test.dc", "reopened", 3);
    CHECK_EQ(reopened, fid);
    CHECK(!sf->is_closed());
    CHECK_EQ(sf->text(), "reopened");
    CHECK_EQ(sf->version(), 3);

    auto update = mgr.update_in_memory("file:///test.dc", "edited", 4);
    CHECK(update.has_value());
    CHECK_EQ(sf->text(), "edited");
    CHECK_EQ(sf->version(), 4);
}

SECTION("sm: position encoding modes");

namespace
{
    std::string const kMixedText = "a\xC3\xA9\xE4\xB8\xAD\xF0\x9F\x98\x80";
} // namespace

TEST_CASE("position encoding defaults to UTF-16 on the SourceManager")
{
    sm::SourceManager mgr;
    CHECK(mgr.position_encoding() == sm::PositionEncoding::Utf16);

    auto fid = mgr.open_in_memory("file:///t.dc", kMixedText, 1);
    REQUIRE(fid != sm::FileId::Invalid);

    auto pos = mgr.location_to_lsp_position(sm::Location{fid, 6});
    REQUIRE(pos.has_value());
    CHECK_EQ(pos->line, 0u);
    CHECK_EQ(pos->character, 3u);
}

TEST_CASE("location_to_lsp_position counts units per negotiated encoding")
{
    sm::SourceManager mgr;
    auto fid = mgr.open_in_memory("file:///t.dc", kMixedText, 1);
    REQUIRE(fid != sm::FileId::Invalid);

    auto const* sf = mgr.get(fid);
    REQUIRE(sf != nullptr);
    CHECK_EQ(sf->size(), 10u);

    struct Case
    {
        sm::PositionEncoding enc;
        std::uint32_t offset;
        std::uint32_t expect_char;
    };

    std::vector<Case> const utf8_cases = {
        {sm::PositionEncoding::Utf8, 0u, 0u}, {sm::PositionEncoding::Utf8, 1u, 1u},   {sm::PositionEncoding::Utf8, 3u, 3u},
        {sm::PositionEncoding::Utf8, 6u, 6u}, {sm::PositionEncoding::Utf8, 10u, 10u},
    };
    std::vector<Case> const utf16_cases = {
        {sm::PositionEncoding::Utf16, 0u, 0u}, {sm::PositionEncoding::Utf16, 1u, 1u},  {sm::PositionEncoding::Utf16, 3u, 2u},
        {sm::PositionEncoding::Utf16, 6u, 3u}, {sm::PositionEncoding::Utf16, 10u, 5u},
    };
    std::vector<Case> const utf32_cases = {
        {sm::PositionEncoding::Utf32, 0u, 0u}, {sm::PositionEncoding::Utf32, 1u, 1u},  {sm::PositionEncoding::Utf32, 3u, 2u},
        {sm::PositionEncoding::Utf32, 6u, 3u}, {sm::PositionEncoding::Utf32, 10u, 4u},
    };

    auto check = [&](std::vector<Case> const& cases) {
        for (auto const& c : cases)
        {
            mgr.set_position_encoding(c.enc);
            auto pos = mgr.location_to_lsp_position(sm::Location{fid, c.offset});
            REQUIRE(pos.has_value());
            CHECK_EQ(pos->line, 0u);
            CHECK_EQ(pos->character, c.expect_char);
        }
    };

    check(utf8_cases);
    check(utf16_cases);
    check(utf32_cases);
}

TEST_CASE("lsp_position_to_location advances the requested number of units")
{
    sm::SourceManager mgr;
    auto fid = mgr.open_in_memory("file:///t.dc", kMixedText, 1);
    REQUIRE(fid != sm::FileId::Invalid);

    mgr.set_position_encoding(sm::PositionEncoding::Utf8);
    CHECK_EQ(mgr.lsp_position_to_location(fid, 0u, 1u)->offset, 1u);
    CHECK_EQ(mgr.lsp_position_to_location(fid, 0u, 3u)->offset, 3u);
    CHECK_EQ(mgr.lsp_position_to_location(fid, 0u, 10u)->offset, 10u);

    mgr.set_position_encoding(sm::PositionEncoding::Utf16);
    CHECK_EQ(mgr.lsp_position_to_location(fid, 0u, 1u)->offset, 1u);
    CHECK_EQ(mgr.lsp_position_to_location(fid, 0u, 2u)->offset, 3u);
    CHECK_EQ(mgr.lsp_position_to_location(fid, 0u, 3u)->offset, 6u);
    CHECK_EQ(mgr.lsp_position_to_location(fid, 0u, 5u)->offset, 10u);

    mgr.set_position_encoding(sm::PositionEncoding::Utf32);
    CHECK_EQ(mgr.lsp_position_to_location(fid, 0u, 1u)->offset, 1u);
    CHECK_EQ(mgr.lsp_position_to_location(fid, 0u, 2u)->offset, 3u);
    CHECK_EQ(mgr.lsp_position_to_location(fid, 0u, 3u)->offset, 6u);
    CHECK_EQ(mgr.lsp_position_to_location(fid, 0u, 4u)->offset, 10u);
}

TEST_CASE("round trips through the negotiated encoding reproduce byte offsets")
{
    sm::SourceManager mgr;
    auto fid = mgr.open_in_memory("file:///t.dc", kMixedText, 1);
    REQUIRE(fid != sm::FileId::Invalid);

    for (auto enc : {sm::PositionEncoding::Utf8, sm::PositionEncoding::Utf16, sm::PositionEncoding::Utf32})
    {
        mgr.set_position_encoding(enc);
        for (std::uint32_t byte : {0u, 1u, 3u, 6u, 10u})
        {
            auto pos = mgr.location_to_lsp_position(sm::Location{fid, byte});
            REQUIRE(pos.has_value());
            auto loc = mgr.lsp_position_to_location(fid, *pos);
            REQUIRE(loc.has_value());
            CHECK_EQ(loc->offset, byte);
        }
    }
}

TEST_CASE("a unit offset inside a multibyte codepoint clamps to the codepoint START")
{
    sm::SourceManager mgr;
    auto fid = mgr.open_in_memory("file:///t.dc", kMixedText, 1);
    REQUIRE(fid != sm::FileId::Invalid);

    mgr.set_position_encoding(sm::PositionEncoding::Utf16);
    auto loc = mgr.lsp_position_to_location(fid, 0u, 4u);
    REQUIRE(loc.has_value());
    CHECK_EQ(loc->offset, 6u);

    mgr.set_position_encoding(sm::PositionEncoding::Utf8);
    loc = mgr.lsp_position_to_location(fid, 0u, 2u);
    REQUIRE(loc.has_value());
    CHECK_EQ(loc->offset, 1u);

    loc = mgr.lsp_position_to_location(fid, 0u, 4u);
    REQUIRE(loc.has_value());
    CHECK_EQ(loc->offset, 3u);

    loc = mgr.lsp_position_to_location(fid, 0u, 7u);
    REQUIRE(loc.has_value());
    CHECK_EQ(loc->offset, 6u);

    mgr.set_position_encoding(sm::PositionEncoding::Utf32);
    loc = mgr.lsp_position_to_location(fid, 0u, 4u);
    REQUIRE(loc.has_value());
    CHECK_EQ(loc->offset, 10u);
}

TEST_CASE("positions past EOL clamp to the end of the line content")
{
    sm::SourceManager mgr;
    auto fid = mgr.open_in_memory("file:///t.dc", kMixedText, 1);
    REQUIRE(fid != sm::FileId::Invalid);

    for (auto enc : {sm::PositionEncoding::Utf8, sm::PositionEncoding::Utf16, sm::PositionEncoding::Utf32})
    {
        mgr.set_position_encoding(enc);
        auto loc = mgr.lsp_position_to_location(fid, 0u, 1000u);
        REQUIRE(loc.has_value());
        CHECK_EQ(loc->offset, 10u);
    }

    auto fid2 = mgr.open_in_memory("file:///crlf.dc", "ab\r\ncd", 1);
    REQUIRE(fid2 != sm::FileId::Invalid);
    mgr.set_position_encoding(sm::PositionEncoding::Utf16);
    auto loc = mgr.lsp_position_to_location(fid2, 0u, 100u);
    REQUIRE(loc.has_value());
    CHECK_EQ(loc->offset, 2u);
    loc = mgr.lsp_position_to_location(fid2, 1u, 100u);
    REQUIRE(loc.has_value());
    CHECK_EQ(loc->offset, 6u);
}

TEST_CASE("an invalid byte offset inside a codepoint is rejected for location->position")
{
    sm::SourceManager mgr;
    auto fid = mgr.open_in_memory("file:///t.dc", kMixedText, 1);
    REQUIRE(fid != sm::FileId::Invalid);

    auto pos = mgr.location_to_lsp_position(sm::Location{fid, 1});
    REQUIRE(pos.has_value());
    CHECK_EQ(pos->character, 1u);

    pos = mgr.location_to_lsp_position(sm::Location{fid, 2});
    CHECK(!pos.has_value());
    CHECK_EQ(pos.error(), sm::Error::InvalidUtf8);

    pos = mgr.location_to_lsp_position(sm::Location{fid, 7});
    CHECK(!pos.has_value());
    CHECK_EQ(pos.error(), sm::Error::InvalidUtf8);

    pos = mgr.location_to_lsp_position(sm::Location{fid, 3});
    REQUIRE(pos.has_value());
    CHECK_EQ(pos->character, 2u);
}

TEST_CASE("line index recognizes LF, CRLF, and standalone CR terminators")
{
    sm::SourceManager mgr;

    auto fid = mgr.open_in_memory("file:///lf.dc", "a\nb", 1);
    REQUIRE(fid != sm::FileId::Invalid);
    auto const* sf = mgr.get(fid);
    REQUIRE(sf != nullptr);
    CHECK_EQ(sf->line_count(), 2u);
    CHECK_EQ(sf->line_text(1), "a");
    CHECK_EQ(sf->line_text(2), "b");

    fid = mgr.open_in_memory("file:///crlf.dc", "a\r\nb", 1);
    sf = mgr.get(fid);
    REQUIRE(sf != nullptr);
    CHECK_EQ(sf->line_count(), 2u);
    CHECK_EQ(sf->line_text(1), "a");
    CHECK_EQ(sf->line_text(2), "b");

    fid = mgr.open_in_memory("file:///cr.dc", "a\rb", 1);
    sf = mgr.get(fid);
    REQUIRE(sf != nullptr);
    CHECK_EQ(sf->line_count(), 2u);
    CHECK_EQ(sf->line_text(1), "a");
    CHECK_EQ(sf->line_text(2), "b");

    fid = mgr.open_in_memory("file:///mixed.dc", "a\r\nb\rc\nd", 1);
    sf = mgr.get(fid);
    REQUIRE(sf != nullptr);
    CHECK_EQ(sf->line_count(), 4u);
    CHECK_EQ(sf->line_text(1), "a");
    CHECK_EQ(sf->line_text(2), "b");
    CHECK_EQ(sf->line_text(3), "c");
    CHECK_EQ(sf->line_text(4), "d");
}

TEST_CASE("empty lines and trailing newline produce correct positions")
{
    sm::SourceManager mgr;
    auto fid = mgr.open_in_memory("file:///t.dc", "a\n\nb", 1);
    REQUIRE(fid != sm::FileId::Invalid);

    auto const* sf = mgr.get(fid);
    REQUIRE(sf != nullptr);
    CHECK_EQ(sf->line_count(), 3u);

    auto pos = mgr.location_to_lsp_position(sm::Location{fid, 2});
    REQUIRE(pos.has_value());
    CHECK_EQ(pos->line, 1u);
    CHECK_EQ(pos->character, 0u);

    auto loc = mgr.lsp_position_to_location(fid, 1u, 0u);
    REQUIRE(loc.has_value());
    CHECK_EQ(loc->offset, 2u);
    loc = mgr.lsp_position_to_location(fid, 1u, 99u);
    REQUIRE(loc.has_value());
    CHECK_EQ(loc->offset, 2u);

    auto bad = mgr.lsp_position_to_location(fid, 3u, 0u);
    CHECK(!bad.has_value());
    CHECK_EQ(bad.error(), sm::Error::OutOfRange);
    bad = mgr.lsp_position_to_location(fid, 100u, 0u);
    CHECK(!bad.has_value());
    CHECK_EQ(bad.error(), sm::Error::OutOfRange);

    auto fid2 = mgr.open_in_memory("file:///t2.dc", "abc\n", 1);
    REQUIRE(fid2 != sm::FileId::Invalid);
    auto const* sf2 = mgr.get(fid2);
    REQUIRE(sf2 != nullptr);
    CHECK_EQ(sf2->line_count(), 2u);

    auto loc2 = mgr.lsp_position_to_location(fid2, 1u, 0u);
    REQUIRE(loc2.has_value());
    CHECK_EQ(loc2->offset, 4u);
}

TEST_CASE("position conversion scans only the target line (no cross-line bleed)")
{
    sm::SourceManager mgr;
    std::string text = std::string(100, 'x') + "\r\n" + kMixedText;
    auto fid = mgr.open_in_memory("file:///t.dc", text, 1);
    REQUIRE(fid != sm::FileId::Invalid);

    mgr.set_position_encoding(sm::PositionEncoding::Utf16);
    auto loc = mgr.lsp_position_to_location(fid, 1u, 3u); // start of 😀
    REQUIRE(loc.has_value());
    CHECK_EQ(loc->offset, 102u + 6u);

    loc = mgr.lsp_position_to_location(fid, 1u, 4u); // inside 😀 -> clamp to start
    REQUIRE(loc.has_value());
    CHECK_EQ(loc->offset, 102u + 6u);

    loc = mgr.lsp_position_to_location(fid, 1u, 5u); // past EOL -> clamp to EOL
    REQUIRE(loc.has_value());
    CHECK_EQ(loc->offset, 112u);

    auto pos = mgr.location_to_lsp_position(sm::Location{fid, 102u + 6u});
    REQUIRE(pos.has_value());
    CHECK_EQ(pos->line, 1u);
    CHECK_EQ(pos->character, 3u);
}

TEST_CASE("empty file converts trivially in every encoding")
{
    sm::SourceManager mgr;
    auto fid = mgr.open_in_memory("file:///t.dc", "", 1);
    REQUIRE(fid != sm::FileId::Invalid);

    for (auto enc : {sm::PositionEncoding::Utf8, sm::PositionEncoding::Utf16, sm::PositionEncoding::Utf32})
    {
        mgr.set_position_encoding(enc);
        auto pos = mgr.location_to_lsp_position(sm::Location{fid, 0});
        REQUIRE(pos.has_value());
        CHECK_EQ(pos->line, 0u);
        CHECK_EQ(pos->character, 0u);

        auto loc = mgr.lsp_position_to_location(fid, 0u, 0u);
        REQUIRE(loc.has_value());
        CHECK_EQ(loc->offset, 0u);
    }
}

TEST_CASE("SourceFile low-level conversion accepts an explicit encoding argument")
{
    sm::SourceManager mgr;
    auto fid = mgr.open_in_memory("file:///t.dc", kMixedText, 1);
    REQUIRE(fid != sm::FileId::Invalid);
    auto const* sf = mgr.get(fid);
    REQUIRE(sf != nullptr);

    auto pos = sf->lsp_position(6, sm::PositionEncoding::Utf8);
    REQUIRE(pos.has_value());
    CHECK_EQ(pos->character, 6u);

    auto offset = sf->offset_at_lsp_position(0, 3, sm::PositionEncoding::Utf8);
    REQUIRE(offset.has_value());
    CHECK_EQ(*offset, 3u);

    offset = sf->offset_at_lsp_position(0, 2, sm::PositionEncoding::Utf8);
    REQUIRE(offset.has_value());
    CHECK_EQ(*offset, 1u);

    pos = sf->lsp_position(6);
    REQUIRE(pos.has_value());
    CHECK_EQ(pos->character, 3u);
}
