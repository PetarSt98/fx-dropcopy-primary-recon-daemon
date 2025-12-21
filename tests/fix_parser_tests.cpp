#include <gtest/gtest.h>
#include <string>
#include <string_view>
#include "ingest/fix_parser.hpp"
#include "util/soh.hpp"

namespace {

TEST(FixParserTest, ParseExecReportOk) {
    const std::string msg = util::pipe_to_soh(
        "8=FIX.4.4|35=8|150=2|39=2|17=E1|11=C1|37=O1|31=1000000|32=200|14=200|52=1|60=2|");
    core::ExecEvent evt{};

    ASSERT_EQ(ingest::parse_exec_report(msg.data(), msg.size(), evt), ingest::ParseResult::Ok);

    EXPECT_EQ(evt.price_micro, 1'000'000);
    EXPECT_EQ(evt.qty, 200);
    EXPECT_EQ(evt.cum_qty, 200);
    EXPECT_EQ(evt.exec_id_len, 2);
    EXPECT_EQ(std::string_view(evt.exec_id, evt.exec_id_len), "E1");
}

TEST(FixParserTest, MissingRequiredField) {
    const std::string msg = util::pipe_to_soh(
        "8=FIX.4.4|35=8|150=2|39=2|17=E1|31=1000000|32=200|14=200|52=1|60=2|");
    core::ExecEvent evt{};

    EXPECT_EQ(ingest::parse_exec_report(msg.data(), msg.size(), evt), ingest::ParseResult::MissingField);
}

} // namespace
