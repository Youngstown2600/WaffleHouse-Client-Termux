#include "trunkmonkey/TextPool.h"
#include <cctype>
#include <fstream>
#include <stdexcept>
namespace trunkmonkey {
namespace {
std::string trim(std::string s){
    while(!s.empty()&&std::isspace((unsigned char)s.front()))s.erase(s.begin());
    while(!s.empty()&&std::isspace((unsigned char)s.back()))s.pop_back();
    return s;
}
}
void TextPool::load(const std::string& path){
    std::ifstream in(path); if(!in) throw std::runtime_error("Unable to open list: "+path);
    std::vector<std::string> v; std::string line;
    while(std::getline(in,line)){ line=trim(line); if(line.empty()||line[0]=='#')continue; v.push_back(line); }
    set(std::move(v));
}
void TextPool::set(std::vector<std::string> v){std::lock_guard<std::mutex> l(mutex_);values_=std::move(v);cursor_=0;}
std::vector<std::string> TextPool::values()const{std::lock_guard<std::mutex> l(mutex_);return values_;}
std::string TextPool::next(){std::lock_guard<std::mutex> l(mutex_);if(values_.empty())return{};auto v=values_[cursor_%values_.size()];cursor_=(cursor_+1)%values_.size();return v;}
bool TextPool::empty()const{std::lock_guard<std::mutex> l(mutex_);return values_.empty();}
std::size_t TextPool::size()const{std::lock_guard<std::mutex> l(mutex_);return values_.size();}
void TextPool::reset(){std::lock_guard<std::mutex> l(mutex_);cursor_=0;}
} // namespace trunkmonkey
