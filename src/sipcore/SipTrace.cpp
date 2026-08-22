#include "trunkmonkey/SipTrace.h"
#include <algorithm>
#include <cctype>
namespace trunkmonkey {
namespace {
std::string upper(std::string s){std::transform(s.begin(),s.end(),s.begin(),[](unsigned char c){return (char)std::toupper(c);});return s;}
}
void classifySipTraceEntry(SipTraceEntry&e,SipTraceClassifierState&state){
    e.method=upper(e.method);
    bool reinvite=false;
    if(e.method=="INVITE"){
        if(e.statusCode==0){
            if(e.inDialogRequest){state.reInviteCseqs.insert(e.cseq);reinvite=true;}
            else if(state.initialInviteCseq==0)state.initialInviteCseq=e.cseq;
        }else reinvite=state.reInviteCseqs.count(e.cseq)!=0;
    }
    if(e.statusCode>0){
        e.label=std::to_string(e.statusCode)+(e.reason.empty()?std::string{}:" "+e.reason);
        if(!e.method.empty())e.label+=" ("+std::string(reinvite?"RE-INVITE":e.method)+")";
    }else e.label=reinvite?"RE-INVITE":(e.method.empty()?"SIP":e.method);
}
}
