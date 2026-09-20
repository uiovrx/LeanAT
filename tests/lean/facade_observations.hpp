#pragma once
#include "Facade.hpp"
#include <fstream>
#include <iomanip>
#include <iostream>
inline void facade_observations(int argc,char**argv,const char* scenario,const std::string& observations) {
  using namespace leanat_generated;
  leanat::exec::LoadPolicy wrong;
  wrong.expected_profile=generated_profile=="AT-Core-1.1-draft"?"AT-Ext-1.1-draft":"AT-Core-1.1-draft";
  if(leanat::exec::load_descriptor(descriptor_bytes(),wrong))
    throw std::runtime_error("wrong expected profile accepted");
  std::ostringstream hash;
  for(auto byte:leanat::exec::sha256(descriptor_bytes()))hash<<std::hex<<std::setw(2)<<std::setfill('0')<<unsigned(byte);
  std::string json="{\"scenario\":\""+std::string(scenario)+"\",\"profile\":\""+generated_profile+
    "\",\"descriptorHash\":\""+hash.str()+"\",\"wrongProfileRejected\":true,\"observations\":"+observations+"}\n";
  if(argc>1){std::ofstream out(argv[1]);if(!out || !(out<<json))throw std::runtime_error("observation write failed");}
  else std::cout<<json;
}
