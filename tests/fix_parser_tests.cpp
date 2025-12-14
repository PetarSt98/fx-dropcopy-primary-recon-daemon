#include "test_main.hpp"
#include "ingest/fix_parser.hpp"
#include "util/soh.hpp"

namespace fix_parser_tests {

bool test_parse_exec_report_ok() {
    const std::string msg = util::pipe_to_soh("8=FIX.4.4|35=8|150=2|39=2|17=E1|11=C1|37=O1|31=1000000|32=200|14=200|52=1|60=2|");
    core::ExecEvent evt{};
    return ingest::parse_exec_report(msg.data(), msg.size(), evt) == ingest::ParseResult::Ok &&
           evt.price_micro == 1000000 && evt.qty == 200 && evt.cum_qty == 200 && evt.exec_id_len == 2;
}

bool test_missing_required_field() {
    const std::string msg = util::pipe_to_soh("8=FIX.4.4|35=8|150=2|39=2|17=E1|31=1000000|32=200|14=200|52=1|60=2|");
    core::ExecEvent evt{};
    return ingest::parse_exec_report(msg.data(), msg.size(), evt) == ingest::ParseResult::MissingField;
}

void add_tests(std::vector<TestCase>& tests) {
    tests.push_back({"parse_exec_report_ok", test_parse_exec_report_ok});
    tests.push_back({"missing_required_field", test_missing_required_field});
}

} // namespace fix_parser_tests
