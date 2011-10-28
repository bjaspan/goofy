#ifndef URL_HH_
#define URL_HH_    
#include <string>
struct url {
    url(const std::string& url_s);
    const std::string& full() { return url_; }
    const std::string& host() { return host_; }
    const std::string& request() { return request_; }
    int port() { return port_; }
private:
    void parse(const std::string& url_s);
private:
    std::string url_, protocol_, host_, path_, query_, request_;
    int port_;
};
#endif /* URL_HH_ */
