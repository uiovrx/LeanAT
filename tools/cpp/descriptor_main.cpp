#include "leanat/descriptor.hpp"
#include "leanat/interpreter.hpp"
#include <charconv>
#include <fstream>
#include <iostream>
#include <iomanip>
#include <sstream>

static std::string quoted(const std::string& text) {
  std::ostringstream out; out << '"';
  for (unsigned char ch : text) {
    if (ch == '"' || ch == '\\') out << '\\' << ch;
    else if (ch < 32) out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << unsigned(ch);
    else out << ch;
  }
  out << '"'; return out.str();
}
static void value_json(std::ostream& out,const leanat::Value& value) {
  const auto& data=value.data;
  if(std::holds_alternative<std::monostate>(data)) out << "{\"kind\":\"unit\",\"value\":null}";
  else if(auto p=std::get_if<bool>(&data)) out << "{\"kind\":\"bool\",\"value\":" << (*p?"true":"false") << '}';
  else if(auto p=std::get_if<std::uint64_t>(&data)) out << "{\"kind\":\"u64\",\"value\":\"" << *p << "\"}";
  else if(auto p=std::get_if<std::int64_t>(&data)) out << "{\"kind\":\"i64\",\"value\":\"" << *p << "\"}";
  else if(auto p=std::get_if<leanat::Bytes>(&data)) {out << "{\"kind\":\"bytes\",\"value\":[";bool first=true;for(auto x:*p){if(!first)out<<',';first=false;out<<unsigned(x);}out<<"]}";}
  else if(auto p=std::get_if<leanat::Value::Array>(&data)) {out << "{\"kind\":\"array\",\"value\":[";bool first=true;for(const auto& x:*p){if(!first)out<<',';first=false;value_json(out,x);}out<<"]}";}
  else throw std::runtime_error("handle output JSON is not supported by scalar segment harness");
}
static std::uint64_t number(const char* text) {
  std::uint64_t result{}; const std::string input(text);
  auto parsed=std::from_chars(input.data(),input.data()+input.size(),result);
  if(input.empty()||parsed.ec!=std::errc{}||parsed.ptr!=input.data()+input.size()) throw std::invalid_argument("expected UInt64 decimal argument");
  return result;
}
int leanat_descriptor_main(int argc,char** argv) {
  const bool run=argc>=2&&std::string(argv[1])=="--run";
  if((!run&&argc!=2)||(run&&argc<5)) { std::cerr << "usage: leanat_descriptor model.execir.bin\n       leanat_descriptor --run model.execir.bin programId fuel [uint64 inputs...]\n"; return 2; }
  try {
    leanat::exec::LoadPolicy policy;
    std::ifstream file(argv[run?2:1], std::ios::binary);
    if(!file) { std::cerr << "{\"error\":\"MissingDescriptor\"}\n"; return 5; }
    leanat::Bytes bytes;
    bytes.reserve(policy.max_file_bytes);
    char ch;
    while(file.get(ch)) {
      if(bytes.size()==policy.max_file_bytes) { std::cerr << "{\"error\":\"ReadLimit\"}\n"; return 2; }
      bytes.push_back(static_cast<unsigned char>(ch));
    }
    if(!file.eof()) { std::cerr << "{\"error\":\"ReadFailure\"}\n"; return 5; }
    auto result=leanat::exec::load_descriptor(bytes,policy);
    if(!result) { std::cerr << "{\"error\":\"InvalidDescriptor\",\"detail\":" << quoted(result.error().message) << "}\n"; return 3; }
    const auto& project=result.value().get();
    if(run) {
      auto program=number(argv[3]); const auto fuel=number(argv[4]);
      if(program>UINT32_MAX) throw std::invalid_argument("programId exceeds UInt32");
      std::vector<leanat::Value> inputs;
      for(int i=5;i<argc;++i) inputs.emplace_back(number(argv[i]));
      std::vector<leanat::VersionedCell> cells; cells.reserve(project.initial_state.size());
      for(const auto& initial:project.initial_state) cells.push_back({initial});
      std::vector<leanat::VersionedCell*> pointers;for(auto& cell:cells)pointers.push_back(&cell);
      leanat::ExecutionContext context;
      leanat::SegmentBudget budget;budget.writes=project.state_types.size();budget.bytes=policy.memory_budget;
      leanat::EventTxn transaction(budget,context);
      leanat::exec::FuelCounter counter{fuel};
      leanat::exec::Interpreter interpreter(result.value(),pointers);
      auto executed=interpreter.execute_segment(static_cast<std::uint32_t>(program),context,inputs,transaction,counter);
      bool complete=false; std::string reason="Failed",error;
      if(!executed) {reason=executed.error().code==leanat::ErrorCode::FuelExhausted?"FuelExhausted":"Failed";error=executed.error().message;}
      else if(executed.value().kind==leanat::exec::SegmentResult::Kind::Failed) error=executed.value().error;
      else if(executed.value().kind!=leanat::exec::SegmentResult::Kind::Returned) {reason="UnsupportedSegmentExit";error="closed scalar harness requires Returned, not suspension or transport return";}
      else {auto committed=transaction.commit();if(!committed)error=committed.error().message;else if(!committed.value().actions.empty())error="external actions unsupported by scalar harness";else {complete=true;reason="Completed";}}
      std::cout << "{\"scope\":\"segment\",\"stopReason\":" << quoted(reason) << ",\"complete\":" << (complete?"true":"false") << ",\"fuelUsed\":\"" << fuel-counter.remaining << "\",\"result\":[";
      bool first=true;if(complete)for(const auto& value:executed.value().values){if(!first)std::cout<<',';first=false;value_json(std::cout,value);}
      std::cout << "],\"state\":[";first=true;for(const auto& cell:cells){if(!first)std::cout<<',';first=false;value_json(std::cout,cell.value);}
      std::cout << "],\"error\":" << quoted(error) << "}\n";
      return complete?0:4;
    }
    std::cout << "{\"status\":\"checked\",\"scope\":\"bounded-descriptor-loader\",\"types\":" << project.types.size() << ",\"programs\":" << project.programs.size() << ",\"release_gate\":\"not-evaluated\"}\n";
    return 0;
  } catch(const std::exception& error) {
    std::cerr << "{\"error\":\"HostFailure\",\"detail\":" << quoted(error.what()) << "}\n"; return 5;
  }
}
#ifndef LEANAT_SYSTEMC_DRIVER
int main(int argc,char** argv) { return leanat_descriptor_main(argc,argv); }
#endif
