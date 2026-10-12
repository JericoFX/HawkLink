#include <cpp-httplib/httplib.h>
#include "hawken/client_steam_identity_transport.hpp"
#include <doctest/doctest.h>
#include <array>
#include <atomic>
#include <barrier>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {
namespace identity=hawken::client::identity;
constexpr std::uint64_t public_id=76561197960265729ULL;
constexpr std::string_view body=R"({"SteamAuthTicket":"public-synthetic-fixture-only"})";
struct Probe {
    bool boundary=false,unavailable=false,zero_id=false,fail_headers=false;
    unsigned fail_option_number=0;
    unsigned reads=0,headers=0,options=0,closed=0,opened=0;
    std::uintptr_t next_handle=0x1000;
    HINTERNET fixed_connection=nullptr;
    INTERNET_PORT fixture_port=0;
    std::wstring header;
    bool no_proxy=false,no_redirect=false,header_before_options=false;
    explicit Probe(int port=0):fixture_port(static_cast<INTERNET_PORT>(port)) {}
};
thread_local Probe* current=nullptr;
bool read_identity(void* context,std::uint64_t* value) noexcept {
    auto& probe=*static_cast<Probe*>(context); ++probe.reads;
    *value=probe.zero_id ? 0 : public_id;
    return !probe.unavailable;
}
HINTERNET WINAPI connect(HINTERNET session,LPCWSTR host,INTERNET_PORT port,DWORD reserved) {
    if (!current->boundary) {
        // Transport still classifies the ORIGINAL logical host/port. Only its
        // injected native adapter redirects these local fixtures physically.
        const bool fixture_host=host && (std::wstring_view(host)==L"127.0.0.1"
            || std::wstring_view(host)==L"localhost");
        const auto physical_port=current->fixture_port && fixture_host && (port==0 || port==80)
            ? current->fixture_port : port;
        return WinHttpConnect(session,host,physical_port,reserved);
    }
    if (current->fixed_connection) return current->fixed_connection;
    return reinterpret_cast<HINTERNET>(++current->next_handle);
}
HINTERNET WINAPI open_request(HINTERNET connection,LPCWSTR verb,LPCWSTR path,
    LPCWSTR version,LPCWSTR referrer,LPCWSTR* types,DWORD flags) {
    ++current->opened;
    if (!current->boundary) return WinHttpOpenRequest(connection,verb,path,version,referrer,types,flags);
    return reinterpret_cast<HINTERNET>(++current->next_handle);
}
BOOL WINAPI close_handle(HINTERNET handle) {
    ++current->closed;
    return current->boundary ? TRUE : WinHttpCloseHandle(handle);
}
BOOL WINAPI add_headers(HINTERNET request,LPCWSTR header,DWORD length,DWORD flags) {
    ++current->headers;
    current->header_before_options=!current->no_proxy || !current->no_redirect;
    current->header=length==static_cast<DWORD>(-1) ? std::wstring(header) : std::wstring(header,length);
    if (current->fail_headers) return FALSE;
    return current->boundary ? TRUE : WinHttpAddRequestHeaders(request,header,length,flags);
}
BOOL WINAPI set_option(HINTERNET handle,DWORD option,LPVOID buffer,DWORD bytes) {
    ++current->options;
    if (option==WINHTTP_OPTION_PROXY && bytes==sizeof(WINHTTP_PROXY_INFO)) {
        const auto& proxy=*static_cast<WINHTTP_PROXY_INFO*>(buffer);
        current->no_proxy=proxy.dwAccessType==WINHTTP_ACCESS_TYPE_NO_PROXY
            && proxy.lpszProxy==nullptr && proxy.lpszProxyBypass==nullptr;
    }
    if (option==WINHTTP_OPTION_DISABLE_FEATURE && bytes==sizeof(DWORD))
        current->no_redirect=(*static_cast<DWORD*>(buffer)&WINHTTP_DISABLE_REDIRECTS)!=0;
    if (current->fail_option_number==current->options) return FALSE;
    return current->boundary ? TRUE : WinHttpSetOption(handle,option,buffer,bytes);
}
identity::Config config(Probe& probe,std::string uri="http://127.0.0.1") {
    return {std::move(uri),{&connect,&open_request,&close_handle,&add_headers,&set_option},
        {&read_identity,&probe}};
}
struct Scope {
    explicit Scope(Probe& probe) { REQUIRE(current==nullptr); current=&probe; }
    ~Scope() { current=nullptr; }
};
struct Handle {
    HINTERNET value;
    identity::Transport* transport=nullptr;
    ~Handle() { if(value) { if(transport) transport->close_handle(value); else WinHttpCloseHandle(value); } }
    Handle(const Handle&)=delete;
    Handle& operator=(const Handle&)=delete;
    explicit Handle(HINTERNET handle,identity::Transport* owner=nullptr):value(handle),transport(owner) {}
};
class Listener {
public:
    std::atomic<unsigned> redirected{0};
    std::atomic<unsigned> requests{0};
    explicit Listener(bool redirect=false) {
        const auto handler=[this,redirect](const httplib::Request& request,httplib::Response& response) {
            ++requests;
            if(request.path=="/redirect-target") ++redirected;
            response.set_header("X-Fixture-Claim",request.has_header("X-Hawken-SteamID")
                ? request.get_header_value("X-Hawken-SteamID") : "none");
            if(redirect && request.path=="/steam/login") {
                response.status=302;
                response.set_header("Location","http://127.0.0.1:"+std::to_string(port_)+"/redirect-target");
            } else response.set_content(request.body,"application/json");
        };
        server_.Get(".*",handler); server_.Post(".*",handler);
        // Always isolated from a live backend on80. Logical port0/80 policy
        // remains in Transport; the test adapter maps it to this real listener.
        port_=server_.bind_to_any_port("127.0.0.1");
        if(port_<1) throw std::runtime_error("Loopback fixture port unavailable.");
        worker_=std::jthread([this]{server_.listen_after_bind();});
        server_.wait_until_ready();
        if(!server_.is_running()) { server_.stop(); throw std::runtime_error("Loopback fixture did not start."); }
    }
    ~Listener() { server_.stop(); }
    int port() const noexcept { return port_; }
private:
    httplib::Server server_;
    int port_=-1;
    std::jthread worker_;
};
struct Response { DWORD status; std::wstring claim; std::string body; };
Response send(HINTERNET request) {
    REQUIRE(WinHttpSendRequest(request,nullptr,0,const_cast<char*>(body.data()),
        static_cast<DWORD>(body.size()),static_cast<DWORD>(body.size()),0));
    REQUIRE(WinHttpReceiveResponse(request,nullptr));
    Response response{}; DWORD size=sizeof(response.status);
    REQUIRE(WinHttpQueryHeaders(request,WINHTTP_QUERY_STATUS_CODE|WINHTTP_QUERY_FLAG_NUMBER,
        nullptr,&response.status,&size,nullptr));
    std::array<wchar_t,128> text{}; size=static_cast<DWORD>(sizeof(text));
    REQUIRE(WinHttpQueryHeaders(request,WINHTTP_QUERY_CUSTOM,L"X-Fixture-Claim",text.data(),&size,nullptr));
    response.claim=text.data();
    for(;;) {
        std::array<char,512> buffer{}; DWORD count=0;
        REQUIRE(WinHttpReadData(request,buffer.data(),static_cast<DWORD>(buffer.size()),&count));
        if(count==0) break;
        response.body.append(buffer.data(),count);
    }
    return response;
}
Handle session() {
    // Fixture itself remains loopback-only even in the RED passthrough stub;
    // a separate named-proxy test exercises overriding a real local proxy.
    Handle handle(WinHttpOpen(L"HawkenPublicIdentityFixture/1.0",WINHTTP_ACCESS_TYPE_NO_PROXY,
        nullptr,nullptr,0));
    REQUIRE(handle.value);
    REQUIRE(WinHttpSetTimeouts(handle.value,2000,2000,2000,2000));
    // Guaranteed copy elision applies to the return expression, not named
    // local; explicit move-free handle construction avoids ownership copies.
    const auto raw=handle.value; handle.value=nullptr;
    return Handle(raw);
}
void expect_configuration_error(const identity::Config& conf) {
    try { identity::Transport transport(conf); }
    catch(const identity::ConfigurationError& error) {
        CHECK(std::string(error.what())=="Invalid client identity transport configuration."); return;
    }
    catch(...) { FAIL_CHECK("wrong identity transport configuration error"); return; }
    FAIL_CHECK("identity transport accepted missing function");
}
}

TEST_CASE("Identity transport real HTTP exact login header and unchanged original body") {
    Listener listener; Probe probe(listener.port()); Scope scope(probe); identity::Transport transport(config(probe));
    auto sess=session(); Handle conn(transport.connect(sess.value,L"127.0.0.1",0,0),&transport); REQUIRE(conn.value);
    Handle request(transport.open_request(conn.value,L"POST",L"/steam/login",nullptr,nullptr,nullptr,0),&transport);
    REQUIRE(request.value); const auto response=send(request.value);
    CHECK(response.status==200); CHECK(response.claim==L"76561197960265729"); CHECK(response.body==body);
    CHECK(probe.reads==1); CHECK(probe.headers==1); CHECK(probe.no_proxy); CHECK(probe.no_redirect);
    CHECK_FALSE(probe.header_before_options); CHECK(probe.options==2);
    CHECK(listener.requests==1); CHECK(listener.port()!=80);
    CHECK(probe.header==L"X-Hawken-SteamID: 76561197960265729\r\n");
}
TEST_CASE("Identity transport real HTTP exact path excludes slash query case and other routes") {
    Listener listener; Probe probe(listener.port()); Scope scope(probe); identity::Transport transport(config(probe)); auto sess=session();
    Handle conn(transport.connect(sess.value,L"127.0.0.1",80,0),&transport); REQUIRE(conn.value);
    for(auto path:std::array<LPCWSTR,5>{L"/steam/login/",L"/steam/login?x=1",L"/Steam/login",L"/version",L"/steam/create-account"}) {
        Handle request(transport.open_request(conn.value,L"POST",path,nullptr,nullptr,nullptr,0),&transport);
        REQUIRE(request.value); const auto response=send(request.value);
        CHECK(response.status==200); CHECK(response.claim==L"none"); CHECK(response.body==body);
    }
    CHECK(probe.reads==0); CHECK(probe.headers==0); CHECK(probe.options==0);
    CHECK(listener.requests==5);
}
TEST_CASE("Identity transport real HTTP GET login is passthrough without identity") {
    Listener listener; Probe probe(listener.port()); Scope scope(probe); identity::Transport transport(config(probe)); auto sess=session();
    Handle conn(transport.connect(sess.value,L"127.0.0.1",80,0),&transport); REQUIRE(conn.value);
    Handle request(transport.open_request(conn.value,L"GET",L"/steam/login",nullptr,nullptr,nullptr,0),&transport);
    REQUIRE(request.value); const auto response=send(request.value);
    CHECK(response.claim==L"none"); CHECK(probe.reads==0); CHECK(probe.options==0); CHECK(probe.headers==0);
    CHECK(listener.requests==1);
}
TEST_CASE("Identity transport real HTTP localhost alias is not the exact literal host") {
    Listener listener; Probe probe(listener.port()); Scope scope(probe); identity::Transport transport(config(probe)); auto sess=session();
    Handle conn(transport.connect(sess.value,L"localhost",80,0),&transport); REQUIRE(conn.value);
    Handle request(transport.open_request(conn.value,L"POST",L"/steam/login",nullptr,nullptr,nullptr,0),&transport);
    REQUIRE(request.value); CHECK(send(request.value).claim==L"none"); CHECK(probe.reads==0); CHECK(probe.headers==0);
    CHECK(listener.requests==1);
}
TEST_CASE("Identity transport real HTTP nondefault port reaches fixture without selecting claim") {
    Listener listener; Probe probe(listener.port()); Scope scope(probe);
    identity::Transport transport(config(probe)); auto sess=session();
    REQUIRE(listener.port()!=80);
    Handle conn(transport.connect(sess.value,L"127.0.0.1",static_cast<INTERNET_PORT>(listener.port()),0),&transport);
    REQUIRE(conn.value);
    Handle request(transport.open_request(conn.value,L"POST",L"/steam/login",nullptr,nullptr,nullptr,0),&transport);
    REQUIRE(request.value); const auto response=send(request.value);
    CHECK(response.status==200); CHECK(response.claim==L"none"); CHECK(response.body==body);
    CHECK(listener.requests==1); CHECK(probe.reads==0); CHECK(probe.headers==0); CHECK(probe.options==0);
}
TEST_CASE("Identity transport real HTTP redirect is returned without following") {
    Listener listener(true); Probe probe(listener.port()); Scope scope(probe); identity::Transport transport(config(probe)); auto sess=session();
    Handle conn(transport.connect(sess.value,L"127.0.0.1",80,0),&transport); REQUIRE(conn.value);
    Handle request(transport.open_request(conn.value,L"POST",L"/steam/login",nullptr,nullptr,nullptr,0),&transport);
    REQUIRE(request.value); const auto response=send(request.value);
    CHECK(response.status==302); CHECK(response.claim==L"76561197960265729"); CHECK(listener.redirected==0);
    CHECK(listener.requests==1);
}
TEST_CASE("Identity transport real HTTP selected request overrides local named proxy") {
    Listener listener; Listener proxy; Probe probe(listener.port()); Scope scope(probe);
    identity::Transport transport(config(probe));
    const auto address=L"127.0.0.1:"+std::to_wstring(proxy.port());
    Handle sess(WinHttpOpen(L"HawkenPublicIdentityFixture/1.0",WINHTTP_ACCESS_TYPE_NAMED_PROXY,
        address.c_str(),nullptr,0)); REQUIRE(sess.value);
    REQUIRE(WinHttpSetTimeouts(sess.value,2000,2000,2000,2000));
    Handle conn(transport.connect(sess.value,L"127.0.0.1",80,0),&transport); REQUIRE(conn.value);
    Handle request(transport.open_request(conn.value,L"POST",L"/steam/login",nullptr,nullptr,nullptr,0),&transport);
    REQUIRE(request.value); const auto response=send(request.value);
    CHECK(response.claim==L"76561197960265729"); CHECK(response.body==body);
    CHECK(probe.no_proxy); CHECK(listener.requests==1); CHECK(proxy.requests==0);
}
TEST_CASE("Identity transport missing or zero fixture identity closes selected real request") {
    Listener listener;
    for(bool zero:std::array<bool,2>{false,true}) {
        Probe probe(listener.port()); probe.unavailable=!zero; probe.zero_id=zero; Scope scope(probe);
        identity::Transport transport(config(probe)); auto sess=session();
        Handle conn(transport.connect(sess.value,L"127.0.0.1",80,0),&transport); REQUIRE(conn.value);
        SetLastError(0);
        Handle request(transport.open_request(conn.value,L"POST",L"/steam/login",nullptr,nullptr,nullptr,0),&transport);
        CHECK(request.value==nullptr); CHECK(GetLastError()==ERROR_ACCESS_DENIED);
        CHECK(probe.reads==1); CHECK(probe.headers==0); CHECK(probe.closed==1);
    }
}
TEST_CASE("Identity transport options or header failure closes selected real request") {
    Listener listener;
    for(unsigned failure:std::array<unsigned,3>{1,2,3}) {
        Probe probe(listener.port()); probe.fail_option_number=failure<3 ? failure : 0;
        probe.fail_headers=failure==3; Scope scope(probe);
        identity::Transport transport(config(probe)); auto sess=session();
        Handle conn(transport.connect(sess.value,L"127.0.0.1",80,0),&transport); REQUIRE(conn.value);
        SetLastError(0);
        Handle request(transport.open_request(conn.value,L"POST",L"/steam/login",nullptr,nullptr,nullptr,0),&transport);
        CHECK(request.value==nullptr); CHECK(GetLastError()==ERROR_ACCESS_DENIED); CHECK(probe.closed==1);
        if(failure<3) CHECK(probe.headers==0);
        else { CHECK(probe.headers==1); CHECK_FALSE(probe.header_before_options); }
    }
}
TEST_CASE("Identity transport real Connect quota128 overflow and close recovery") {
    Probe probe; Scope scope(probe); identity::Transport transport(config(probe)); auto sess=session();
    std::vector<HINTERNET> connections;
    for(unsigned i=0;i<128;++i) {
        auto connection=transport.connect(sess.value,L"127.0.0.1",80,0); REQUIRE(connection); connections.push_back(connection);
    }
    SetLastError(0); const auto overflow=transport.connect(sess.value,L"127.0.0.1",80,0);
    CHECK(overflow==nullptr); CHECK(GetLastError()==ERROR_ACCESS_DENIED); CHECK(probe.closed==1);
    // Also dispose a returned stub overflow so RED fixture itself never leaks.
    if(overflow) transport.close_handle(overflow);
    REQUIRE(transport.close_handle(connections.back())); connections.pop_back();
    auto recovered=transport.connect(sess.value,L"127.0.0.1",0,0); CHECK(recovered!=nullptr);
    if(recovered) transport.close_handle(recovered);
    for(auto connection:connections) CHECK(transport.close_handle(connection));
}
TEST_CASE("Identity transport boundary foreign hosts ports and secure requests never call provider") {
    Probe probe; probe.boundary=true; Scope scope(probe); identity::Transport transport(config(probe));
    for(auto host:std::array<LPCWSTR,3>{L"example.invalid",L"127.0.0.2",L"localhost"}) {
        auto conn=transport.connect(nullptr,host,80,0); REQUIRE(conn);
        auto req=transport.open_request(conn,L"POST",L"/steam/login",nullptr,nullptr,nullptr,0); REQUIRE(req);
        CHECK(transport.close_handle(req)); CHECK(transport.close_handle(conn));
    }
    for(auto port:std::array<INTERNET_PORT,2>{static_cast<INTERNET_PORT>(81),static_cast<INTERNET_PORT>(443)}) {
        auto conn=transport.connect(nullptr,L"127.0.0.1",port,0); REQUIRE(conn);
        auto req=transport.open_request(conn,L"POST",L"/steam/login",nullptr,nullptr,nullptr,0); REQUIRE(req);
        CHECK(transport.close_handle(req)); CHECK(transport.close_handle(conn));
    }
    auto conn=transport.connect(nullptr,L"127.0.0.1",80,0); REQUIRE(conn);
    auto req=transport.open_request(conn,L"POST",L"/steam/login",nullptr,nullptr,nullptr,WINHTTP_FLAG_SECURE); REQUIRE(req);
    CHECK(transport.close_handle(req)); CHECK(transport.close_handle(conn));
    CHECK(probe.reads==0); CHECK(probe.headers==0); CHECK(probe.options==0);
}
TEST_CASE("Identity transport boundary method null and unmatched routing root pass through") {
    for(auto root:std::array<const char*,3>{"http://127.0.0.1","http://localhost","https://127.0.0.1"}) {
        Probe probe; probe.boundary=true; Scope scope(probe); identity::Transport transport(config(probe,root));
        auto conn=transport.connect(nullptr,L"127.0.0.1",80,0); REQUIRE(conn);
        for(auto method:std::array<LPCWSTR,4>{nullptr,L"HEAD",L"post",L"PUT"}) {
            auto req=transport.open_request(conn,method,L"/steam/login",nullptr,nullptr,nullptr,0); REQUIRE(req);
            CHECK(transport.close_handle(req));
        }
        if(std::string_view(root)!="http://127.0.0.1") {
            auto req=transport.open_request(conn,L"POST",L"/steam/login",nullptr,nullptr,nullptr,0); REQUIRE(req);
            CHECK(transport.close_handle(req));
        }
        CHECK(transport.close_handle(conn)); CHECK(probe.reads==0); CHECK(probe.headers==0); CHECK(probe.options==0);
    }
}
TEST_CASE("Identity transport foreign connections do not consume local quota") {
    Probe probe; probe.boundary=true; Scope scope(probe); identity::Transport transport(config(probe));
    for(unsigned i=0;i<200;++i) REQUIRE(transport.connect(nullptr,L"example.invalid",80,0));
    for(unsigned i=0;i<128;++i) REQUIRE(transport.connect(nullptr,L"127.0.0.1",80,0));
    SetLastError(0); CHECK(transport.connect(nullptr,L"127.0.0.1",80,0)==nullptr);
    CHECK(GetLastError()==ERROR_ACCESS_DENIED); CHECK(probe.closed==1);
}
TEST_CASE("Identity transport recycled handle reclassifies local to foreign without leaking claim") {
    Probe probe; probe.boundary=true; probe.fixed_connection=reinterpret_cast<HINTERNET>(0x1000u);
    Scope scope(probe); identity::Transport transport(config(probe));
    const auto local=transport.connect(nullptr,L"127.0.0.1",80,0); REQUIRE(local);
    REQUIRE(transport.close_handle(local));
    const auto foreign=transport.connect(nullptr,L"example.invalid",80,0); REQUIRE(foreign==local);
    const auto req=transport.open_request(foreign,L"POST",L"/steam/login",nullptr,nullptr,nullptr,0); REQUIRE(req);
    CHECK(probe.reads==0); CHECK(probe.headers==0); CHECK(probe.options==0);
    CHECK(transport.close_handle(req)); CHECK(transport.close_handle(foreign));
}
TEST_CASE("Identity transport synchronized capacity admits only one contender for last slot") {
    Probe main_probe; main_probe.boundary=true; Scope scope(main_probe);
    identity::Transport transport(config(main_probe)); std::vector<HINTERNET> handles;
    for(unsigned i=0;i<127;++i) {
        auto handle=transport.connect(nullptr,L"127.0.0.1",80,0); REQUIRE(handle); handles.push_back(handle);
    }
    std::barrier start(3),done(3);
    std::atomic<unsigned> admitted{0},denied{0};
    const auto worker=[&](std::uintptr_t first) {
        Probe probe; probe.boundary=true; probe.next_handle=first; current=&probe;
        start.arrive_and_wait(); SetLastError(0);
        auto handle=transport.connect(nullptr,L"127.0.0.1",80,0);
        if(handle) ++admitted;
        else if(GetLastError()==ERROR_ACCESS_DENIED) ++denied;
        // Keep any successful connection until both decisions are observed.
        done.arrive_and_wait();
        if(handle) transport.close_handle(handle);
        current=nullptr;
    };
    std::jthread first(worker,static_cast<std::uintptr_t>(0x10000));
    std::jthread second(worker,static_cast<std::uintptr_t>(0x20000));
    start.arrive_and_wait(); done.arrive_and_wait(); first.join(); second.join();
    CHECK(admitted==1); CHECK(denied==1);
    for(auto handle:handles) CHECK(transport.close_handle(handle));
}
TEST_CASE("Identity transport validates all saved functions and fixture callback") {
    Probe probe;
    auto c=config(probe); c.api.connect=nullptr; expect_configuration_error(c);
    c=config(probe); c.api.open_request=nullptr; expect_configuration_error(c);
    c=config(probe); c.api.close_handle=nullptr; expect_configuration_error(c);
    c=config(probe); c.api.add_headers=nullptr; expect_configuration_error(c);
    c=config(probe); c.api.set_option=nullptr; expect_configuration_error(c);
    c=config(probe); c.identity.read=nullptr; expect_configuration_error(c);
}

// Diagnostic localhost trial policy: reject two GENERIC PlayFab APIs for this
// title. The endpoint does not identify a calendar-only caller or grant rewards.
// Boundary APIs below never perform egress, read a body, or create a real request.
TEST_CASE("Local PlayFab API failure rejects both selected routes before request creation") {
    for(auto port:std::array<INTERNET_PORT,2>{0,443}) {
        for(auto path:std::array<LPCWSTR,2>{L"/Client/GetTitleData",L"/Client/ExecuteCloudScript"}) {
            Probe probe; probe.boundary=true; probe.unavailable=true; Scope scope(probe);
            identity::Transport transport(config(probe));
            Handle conn(transport.connect(nullptr,L"3F09.playfabapi.com",port,0),&transport); REQUIRE(conn.value);
            const auto prior_handle=probe.next_handle;
            SetLastError(0);
            Handle request(transport.open_request(conn.value,L"POST",path,nullptr,nullptr,nullptr,
                WINHTTP_FLAG_SECURE),&transport);
            CHECK(request.value==nullptr); CHECK(GetLastError()==ERROR_WINHTTP_CANNOT_CONNECT);
            CHECK(probe.opened==0); CHECK(probe.next_handle==prior_handle);
            CHECK(probe.reads==0); CHECK(probe.headers==0); CHECK(probe.options==0);
            CHECK(probe.closed==0); // The live connection belongs to its caller.
        }
    }
}
TEST_CASE("Local PlayFab API failure preserves unselected methods paths and secure flags") {
    struct Input { LPCWSTR method; LPCWSTR path; DWORD flags; };
    const std::array<Input,12> inputs{{
        {L"GET",L"/Client/GetTitleData",WINHTTP_FLAG_SECURE},
        {L"post",L"/Client/GetTitleData",WINHTTP_FLAG_SECURE},
        {L"HEAD",L"/Client/ExecuteCloudScript",WINHTTP_FLAG_SECURE},
        {nullptr,L"/Client/ExecuteCloudScript",WINHTTP_FLAG_SECURE},
        {L"POST",L"/Client/GetTitleData",0},
        {L"POST",L"/Client/ExecuteCloudScript",0},
        {L"POST",L"/Client/GetTitleData/",WINHTTP_FLAG_SECURE},
        {L"POST",L"/Client/GetTitleData?fixture=1",WINHTTP_FLAG_SECURE},
        {L"POST",L"/client/GetTitleData",WINHTTP_FLAG_SECURE},
        {L"POST",L"/Client/LoginWithCustomID",WINHTTP_FLAG_SECURE},
        {L"POST",L"/Client/ExecuteCloudScript/",WINHTTP_FLAG_SECURE},
        {L"POST",nullptr,WINHTTP_FLAG_SECURE}
    }};
    Probe probe; probe.boundary=true; Scope scope(probe); identity::Transport transport(config(probe));
    Handle conn(transport.connect(nullptr,L"3F09.playfabapi.com",443,0),&transport); REQUIRE(conn.value);
    for(const auto& input:inputs) {
        const auto prior_opened=probe.opened;
        Handle request(transport.open_request(conn.value,input.method,input.path,nullptr,nullptr,nullptr,
            input.flags),&transport);
        REQUIRE(request.value); CHECK(probe.opened==prior_opened+1);
    }
    CHECK(probe.reads==0); CHECK(probe.headers==0); CHECK(probe.options==0);
}
TEST_CASE("Local PlayFab API failure preserves other hosts ports and routing roots") {
    struct Input { const char* root; LPCWSTR host; INTERNET_PORT port; };
    const std::array<Input,9> inputs{{
        {"http://127.0.0.1",L"3f09.playfabapi.com",443},
        {"http://127.0.0.1",L"OTHER.playfabapi.com",443},
        {"http://127.0.0.1",L"3F09.playfabapi.com.",443},
        {"http://127.0.0.1",nullptr,443},
        {"http://127.0.0.1",L"3F09.playfabapi.com",80},
        {"http://127.0.0.1",L"3F09.playfabapi.com",444},
        {"http://localhost",L"3F09.playfabapi.com",443},
        {"https://127.0.0.1",L"3F09.playfabapi.com",443},
        {"http://127.0.0.1/",L"3F09.playfabapi.com",443}
    }};
    for(const auto& input:inputs) {
        Probe probe; probe.boundary=true; Scope scope(probe);
        identity::Transport transport(config(probe,input.root));
        Handle conn(transport.connect(nullptr,input.host,input.port,0),&transport); REQUIRE(conn.value);
        Handle request(transport.open_request(conn.value,L"POST",L"/Client/ExecuteCloudScript",
            nullptr,nullptr,nullptr,WINHTTP_FLAG_SECURE),&transport);
        REQUIRE(request.value); CHECK(probe.opened==1);
        CHECK(probe.reads==0); CHECK(probe.headers==0); CHECK(probe.options==0);
    }
}
TEST_CASE("Local PlayFab API failure connection class stays distinct from identity local") {
    Probe probe; probe.boundary=true; Scope scope(probe); identity::Transport transport(config(probe));
    Handle api(transport.connect(nullptr,L"3F09.playfabapi.com",443,0),&transport); REQUIRE(api.value);
    Handle local(transport.connect(nullptr,L"127.0.0.1",80,0),&transport); REQUIRE(local.value);
    Handle api_login(transport.open_request(api.value,L"POST",L"/steam/login",nullptr,nullptr,nullptr,0),&transport);
    REQUIRE(api_login.value); CHECK(probe.reads==0); CHECK(probe.headers==0); CHECK(probe.options==0);
    Handle local_api(transport.open_request(local.value,L"POST",L"/Client/GetTitleData",nullptr,nullptr,nullptr,
        WINHTTP_FLAG_SECURE),&transport);
    REQUIRE(local_api.value); CHECK(probe.reads==0); CHECK(probe.headers==0); CHECK(probe.options==0);
    Handle local_login(transport.open_request(local.value,L"POST",L"/steam/login",nullptr,nullptr,nullptr,0),&transport);
    REQUIRE(local_login.value); CHECK(probe.reads==1); CHECK(probe.headers==1); CHECK(probe.options==2);
    CHECK(probe.header==L"X-Hawken-SteamID: 76561197960265729\r\n");
}
TEST_CASE("Local PlayFab API failure shares quota128 and close recovery with identity local") {
    Probe probe; probe.boundary=true; Scope scope(probe); identity::Transport transport(config(probe));
    std::vector<HINTERNET> handles;
    for(unsigned i=0;i<128;++i) {
        auto handle=transport.connect(nullptr,i%2==0 ? L"3F09.playfabapi.com" : L"127.0.0.1",
            i%2==0 ? static_cast<INTERNET_PORT>(443) : static_cast<INTERNET_PORT>(80),0);
        REQUIRE(handle); handles.push_back(handle);
    }
    for(auto host:std::array<LPCWSTR,2>{L"3F09.playfabapi.com",L"127.0.0.1"}) {
        SetLastError(0);
        auto overflow=transport.connect(nullptr,host,0,0);
        CHECK(overflow==nullptr); CHECK(GetLastError()==ERROR_ACCESS_DENIED);
        if(overflow) transport.close_handle(overflow); // Dispose a RED passthrough handle.
    }
    CHECK(probe.closed==2);
    auto foreign=transport.connect(nullptr,L"example.invalid",443,0); REQUIRE(foreign);
    CHECK(transport.close_handle(foreign));
    REQUIRE(transport.close_handle(handles.front())); handles.erase(handles.begin());
    auto recovered=transport.connect(nullptr,L"3F09.playfabapi.com",0,0); REQUIRE(recovered);
    const auto prior_opened=probe.opened;
    SetLastError(0);
    Handle request(transport.open_request(recovered,L"POST",L"/Client/GetTitleData",nullptr,nullptr,nullptr,
        WINHTTP_FLAG_SECURE),&transport);
    CHECK(request.value==nullptr); CHECK(GetLastError()==ERROR_WINHTTP_CANNOT_CONNECT);
    CHECK(probe.opened==prior_opened); CHECK(probe.reads==0); CHECK(probe.headers==0); CHECK(probe.options==0);
    REQUIRE(transport.close_handle(recovered));
    for(auto handle:handles) CHECK(transport.close_handle(handle));
}
TEST_CASE("Local PlayFab API failure recycled handle reclassifies API foreign and identity safely") {
    Probe probe; probe.boundary=true; probe.fixed_connection=reinterpret_cast<HINTERNET>(0x1000u);
    Scope scope(probe); identity::Transport transport(config(probe));
    auto conn=transport.connect(nullptr,L"3F09.playfabapi.com",443,0); REQUIRE(conn);
    const auto failure=[&] {
        const auto prior_opened=probe.opened;
        SetLastError(0);
        Handle request(transport.open_request(conn,L"POST",L"/Client/ExecuteCloudScript",nullptr,nullptr,nullptr,
            WINHTTP_FLAG_SECURE),&transport);
        CHECK(request.value==nullptr); CHECK(GetLastError()==ERROR_WINHTTP_CANNOT_CONNECT);
        CHECK(probe.opened==prior_opened);
    };
    failure();
    // Simulate numeric reuse even without an observed close: the new Connect
    // result must replace stale classification, rather than retaining a gate.
    REQUIRE(transport.connect(nullptr,L"example.invalid",443,0)==conn);
    { Handle request(transport.open_request(conn,L"POST",L"/Client/ExecuteCloudScript",nullptr,nullptr,nullptr,
        WINHTTP_FLAG_SECURE),&transport); REQUIRE(request.value); }
    CHECK(probe.reads==0); CHECK(probe.headers==0); CHECK(probe.options==0);
    REQUIRE(transport.connect(nullptr,L"127.0.0.1",80,0)==conn);
    { Handle request(transport.open_request(conn,L"POST",L"/steam/login",nullptr,nullptr,nullptr,0),&transport);
        REQUIRE(request.value); }
    CHECK(probe.reads==1); CHECK(probe.headers==1); CHECK(probe.options==2);
    REQUIRE(transport.close_handle(conn));
    REQUIRE(transport.connect(nullptr,L"3F09.playfabapi.com",0,0)==conn);
    failure(); CHECK(probe.reads==1); CHECK(probe.headers==1); CHECK(probe.options==2);
    CHECK(transport.close_handle(conn));
}
