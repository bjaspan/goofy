#include "url.hh"
#include <string>
#include <algorithm>
#include <cctype>
#include <functional>
#include <iostream>
using namespace std;

url::url(const std::string &url_s) : url_(url_s) {
    port_ = 0;
    parse(url_s);
}

void url::parse(const string& url_s)
{
    int i;
    
    // Protocol is everything up to "://".
    const string prot_end("://");
    string::const_iterator it = search(url_s.begin(), url_s.end(),
                                           prot_end.begin(), prot_end.end());
    protocol_.reserve(distance(url_s.begin(), it));
    transform(url_s.begin(), it,
              back_inserter(protocol_),
              ptr_fun<int,int>(tolower)); // protocol is icase
    if( it == url_s.end() )
        return;
    advance(it, prot_end.length());

    // [user[:pass]@]host[:port]. user:pass is not yet supported.
    string::const_iterator path_i = find(it, url_s.end(), '/');
    host_.reserve(distance(it, path_i));
    host_.assign(it, path_i);
    if ((i=host_.find(':')) != host_.npos) {
	port_ = atoi(host_.substr(i+1, host_.npos).c_str());
	host_.resize(i);
    }
    else {
	port_ = 80;
    }
    // host is icase
    transform(host_.begin(), host_.end(), host_.begin(), ptr_fun<int,int>(tolower));

    string::const_iterator query_i = find(path_i, url_s.end(), '?');
    request_.assign(path_i, url_s.end());
    path_.assign(path_i, query_i);
    if( query_i != url_s.end() )
        ++query_i;
    query_.assign(query_i, url_s.end());
}

