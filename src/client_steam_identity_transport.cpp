#include "hawken/client_steam_identity_transport.hpp"
#include <charconv>
#include <cwchar>
#include <limits>

namespace hawken::client::identity {
namespace {
HINTERNET fail_created(const WinHttpApi& api,HINTERNET handle) noexcept {
    // Saved native function, not this adapter: no recursive table mutation.
    if(handle) {
        try { (void)api.close_handle(handle); } catch(...) {}
    }
    SetLastError(ERROR_ACCESS_DENIED);
    return nullptr;
}
bool exact(LPCWSTR value,LPCWSTR expected) noexcept {
    return value!=nullptr && std::wcscmp(value,expected)==0;
}
}

ConfigurationError::ConfigurationError()
    : std::runtime_error("Invalid client identity transport configuration.") {}

Transport::Transport(const Config& config) : config_(config) {
    if(!config_.api.connect || !config_.api.open_request || !config_.api.close_handle
        || !config_.api.add_headers || !config_.api.set_option || !config_.identity.read)
        throw ConfigurationError();
}
HINTERNET Transport::connect(HINTERNET session,LPCWSTR host,INTERNET_PORT port,DWORD reserved) noexcept {
    HINTERNET created=nullptr;
    try {
        created=config_.api.connect(session,host,port,reserved);
        if(!created || config_.meteor_uri!="http://127.0.0.1") return created;
        const DWORD original_error=GetLastError();
        const bool local=exact(host,L"127.0.0.1") && (port==0 || port==80);
        const bool playfab=exact(host,L"3F09.playfabapi.com") && (port==0 || port==443);
        std::lock_guard lock(mutex_);
        // A new Connect result supersedes any stale classification with the
        // same numeric handle, including a now-foreign connection.
        Connection* empty=nullptr;
        for(auto& entry:connections_) {
            if(entry.handle==created) entry={};
            if(entry.handle==nullptr && empty==nullptr) empty=&entry;
        }
        if(local || playfab) {
            if(empty==nullptr || generation_==std::numeric_limits<std::uint64_t>::max())
                return fail_created(config_.api,created);
            *empty={created,++generation_,local ? ConnectionKind::identity_local : ConnectionKind::playfab_api};
        }
        SetLastError(original_error);
        return created;
    } catch(...) {
        return fail_created(config_.api,created);
    }
}
HINTERNET Transport::open_request(HINTERNET connection,LPCWSTR verb,LPCWSTR object,
    LPCWSTR version,LPCWSTR referrer,LPCWSTR* accept_types,DWORD flags) noexcept {
    HINTERNET created=nullptr;
    try {
        if(config_.meteor_uri=="http://127.0.0.1" && exact(verb,L"POST")
            && (exact(object,L"/Client/GetTitleData") || exact(object,L"/Client/ExecuteCloudScript"))
            && (flags&WINHTTP_FLAG_SECURE)!=0) {
            std::lock_guard lock(mutex_);
            for(const auto& entry:connections_) {
                if(entry.handle==connection && connection!=nullptr && entry.kind==ConnectionKind::playfab_api) {
                    // Fail before a request exists: the SDK cannot add PlayFab
                    // authentication headers or send its original body here.
                    SetLastError(ERROR_WINHTTP_CANNOT_CONNECT);
                    return nullptr;
                }
            }
        }
        if(config_.meteor_uri!="http://127.0.0.1" || !exact(verb,L"POST")
            || !exact(object,L"/steam/login") || (flags&WINHTTP_FLAG_SECURE)!=0)
            return config_.api.open_request(connection,verb,object,version,referrer,accept_types,flags);
        std::uint64_t generation=0;
        {
            std::lock_guard lock(mutex_);
            for(const auto& entry:connections_)
                if(entry.handle==connection && connection!=nullptr
                    && entry.kind==ConnectionKind::identity_local) generation=entry.generation;
        }
        if(generation==0)
            return config_.api.open_request(connection,verb,object,version,referrer,accept_types,flags);
        created=config_.api.open_request(connection,verb,object,version,referrer,accept_types,flags);
        if(!created) return nullptr;
        const DWORD original_error=GetLastError();
        // Recheck after the real call, which may run concurrently with close
        // and reuse. Hold the lock through the selected operation so our close
        // adapter cannot retire this generation while adding the claim.
        std::lock_guard lock(mutex_);
        bool live=false;
        for(const auto& entry:connections_)
            if(entry.handle==connection && entry.generation==generation
                && entry.kind==ConnectionKind::identity_local) live=true;
        if(!live) return fail_created(config_.api,created);
        std::uint64_t identity=0;
        if(!config_.identity.read(config_.identity.context,&identity) || identity==0)
            return fail_created(config_.api,created);
        WINHTTP_PROXY_INFO proxy{WINHTTP_ACCESS_TYPE_NO_PROXY,nullptr,nullptr};
        DWORD disabled=WINHTTP_DISABLE_REDIRECTS;
        if(!config_.api.set_option(created,WINHTTP_OPTION_PROXY,&proxy,sizeof(proxy))
            || !config_.api.set_option(created,WINHTTP_OPTION_DISABLE_FEATURE,&disabled,sizeof(disabled)))
            return fail_created(config_.api,created);
        char digits[20];
        const auto converted=std::to_chars(digits,digits+sizeof(digits),identity);
        if(converted.ec!=std::errc{}) return fail_created(config_.api,created);
        constexpr wchar_t prefix[]=L"X-Hawken-SteamID: ";
        wchar_t header[64]{};
        std::size_t length=0;
        for(std::size_t i=0;i<sizeof(prefix)/sizeof(wchar_t)-1;++i) header[length++]=prefix[i];
        for(const char* p=digits;p!=converted.ptr;++p) header[length++]=static_cast<wchar_t>(*p);
        header[length++]=L'\r'; header[length++]=L'\n';
        const BOOL added=config_.api.add_headers(created,header,static_cast<DWORD>(length),
            WINHTTP_ADDREQ_FLAG_ADD|WINHTTP_ADDREQ_FLAG_REPLACE);
        SecureZeroMemory(header,sizeof(header));
        SecureZeroMemory(digits,sizeof(digits));
        SecureZeroMemory(&identity,sizeof(identity));
        if(!added) return fail_created(config_.api,created);
        SetLastError(original_error);
        return created;
    } catch(...) {
        return fail_created(config_.api,created);
    }
}
BOOL Transport::close_handle(HINTERNET handle) noexcept {
    try {
        std::lock_guard lock(mutex_);
        const BOOL closed=config_.api.close_handle(handle);
        const DWORD original_error=GetLastError();
        if(closed) for(auto& entry:connections_) if(entry.handle==handle) entry={};
        SetLastError(original_error);
        return closed;
    } catch(...) {
        SetLastError(ERROR_ACCESS_DENIED);
        return FALSE;
    }
}
}
