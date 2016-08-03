
#include "webclient.h"

#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
#pragma comment(lib, "ws2_32.lib")
#if USE_OPENSSL
#pragma comment(lib, "libeay32.lib")
#pragma comment(lib, "ssleay32.lib")
#endif
typedef SOCKET socket_t;
#pragma warning(disable:4996)
#else
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <net/if.h>
#include <netdb.h>
#define closesocket(S) ::close(S)
typedef int socket_t;
#define INVALID_SOCKET (-1)
#define SOCKET_ERROR (-1)
#define stricmp(A, B) strcasecmp(A, B)
#define strnicmp(A, B, C) strncasecmp(A, B, C)
#endif

#if USE_OPENSSL
#include <openssl/crypto.h>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#else
typedef void SSL;
typedef void SSL_CTX;
#endif

#define USER_AGENT "Generic Web Client"

struct WebContext::Private {
	SSL_CTX *ctx;
	bool use_keep_alive = false;
};

WebClient::URL::URL(std::string const &addr)
{
	char const *str = addr.c_str();
	char const *left;
	char const *right;
	left = str;
	right = strstr(left, "://");
	if (right) {
		scheme_.assign(str, right - str);
		left = right + 3;
	}
	right = strchr(left, '/');
	if (right) {
		char const *p = strchr(left, ':');
		if (p && left < p && p < right) {
			int n = 0;
			char const *q = p + 1;
			while (q < right) {
				if (isdigit(*q & 0xff)) {
					n = n * 10 + (*q - '0');
				} else {
					n = -1;
					break;
				}
				q++;
			}
			host_.assign(left, p - left);
			if (n > 0 && n < 65536) {
				port_ = n;
			}
		} else {
			host_.assign(left, right - left);
		}
		path_ = right;
	}
}

bool WebClient::URL::isssl() const
{
	if (scheme() == "https") return true;
	if (scheme() == "http") return false;
	if (port() == 443) return true;
	return false;
}


void WebClientHandler::abort(const std::string &message)
{
	throw WebClient::Error(message);
}

struct WebClient::Private {
	std::vector<std::string> request_header;
	Error error;
	Response response;
	WebContext *webcx;
	int crlf_state = 0;
	size_t content_offset;
	std::string last_host_name;
	bool keep_alive = false;
	socket_t sock = INVALID_SOCKET;
	SSL *ssl = nullptr;
};

WebClient::WebClient(WebContext *webcx)
{
	pv = new Private;
	pv->webcx = webcx;
}

WebClient::~WebClient()
{
	close();
	delete pv;
}

void WebClient::initialize()
{
#ifdef _WIN32
	WSADATA wsaData;
	WORD wVersionRequested;
	wVersionRequested = MAKEWORD(1, 1);
	WSAStartup(wVersionRequested, &wsaData);
	atexit(cleanup);
#endif
#if USE_OPENSSL
	OpenSSL_add_all_algorithms();
#endif
}

void WebClient::cleanup()
{
#if USE_OPENSSL
	ERR_free_strings();
#endif
#ifdef _WIN32
	WSACleanup();
#endif
}

void WebClient::output_debug_string(char const *str)
{
	if (0) {
#ifdef _WIN32
		OutputDebugStringA(str);
#else
		fwrite(str, 1, strlen(str), stderr);
#endif
	}
}

void WebClient::output_debug_strings(std::vector<std::string> const &vec)
{
	for (std::string const &s : vec) {
		output_debug_string((s + '\n').c_str());
	}
}

WebClient::Error const &WebClient::error() const
{
	return pv->error;
}

void WebClient::clear_error()
{
	pv->error = Error();
}

int WebClient::get_port(URL const *uri, char const *scheme, char const *protocol)
{
	int port = uri->port();
	if (port < 1 || port > 65535) {
		struct servent *s;
		s = getservbyname(uri->scheme().c_str(), protocol);
		if (s) {
			port = ntohs(s->s_port);
		} else {
			s = getservbyname(scheme, protocol);
			if (s) {
				port = ntohs(s->s_port);
			}
		}
		if (port < 1 || port > 65535) {
			port = 80;
		}
	}
	return port;
}

static inline std::string to_s(size_t n)
{
	char tmp[100];
	sprintf(tmp, "%u", (int)n);
	return tmp;
}

void WebClient::set_default_header(URL const &uri, Post const *post, RequestOption const &opt)
{
	std::vector<std::string> header;
	auto AddHeader = [&](std::string const &s){
		header.push_back(s);
	};
	AddHeader("Host: " + uri.host());
	AddHeader("User-Agent: " USER_AGENT);
	AddHeader("Accept: */*");
	if (opt.keep_alive) {
		AddHeader("Connection: keep-alive");
	} else {
		AddHeader("Connection: close");
	}
	if (post) {
		AddHeader("Content-Length: " + to_s(post->data.size()));
		std::string ct = "Content-Type: ";
		if (post->content_type.empty()) {
			ct += "application/octet-stream";
		} else if (post->content_type == CT_MULTIPART_FORM_DATA) {
			ct += post->content_type;
			if (!post->boundary.empty()) {
				ct += "; boundary=";
				ct += post->boundary;
			}
		} else {
			ct += post->content_type;
		}
		AddHeader(ct);
	}
	header.insert(header.end(), pv->request_header.begin(), pv->request_header.end());
	pv->request_header = std::move(header);
}

std::string WebClient::make_http_request(URL const &uri, Post const *post)
{
	std::string str;

	str = post ? "POST " : "GET ";
	str += uri.path();
	str += " HTTP/1.0";
	str += "\r\n";

	for (std::string const &s: pv->request_header) {
		str += s;
		str += "\r\n";
	}

	str += "\r\n";
	return str;
}

void WebClient::parse_http_header(char const *begin, char const *end, std::vector<std::string> *header)
{
	if (begin < end) {
		char const *left = begin;
		char const *right = left;
		while (1) {
			if (right >= end) {
				break;
			}
			if (*right == '\r' || *right == '\n') {
				if (left < right) {
					header->push_back(std::string(left, right));
				}
				if (right + 1 < end && *right == '\r' && right[1] == '\n') {
					right++;
				}
				right++;
				if (*right == '\r' || *right == '\n') {
					if (right + 1 < end && *right == '\r' && right[1] == '\n') {
						right++;
					}
					right++;
					left = right;
					break;
				}
				left = right;
			} else {
				right++;
			}
		}
	}
}

void WebClient::parse_http_header(const char *begin, const char *end, WebClient::Response *out)
{
	*out = Response();
	parse_http_header(begin, end, &out->header);
	parse_header(&out->header, out);
}

static void send_(socket_t s, char const *ptr, int len)
{
	while (len > 0) {
		int n = send(s, ptr, len, 0);
		if (n < 1 || n > len) {
			throw WebClient::Error("send request failed.");
		}
		ptr += n;
		len -= n;
	}
}

void WebClient::on_end_header(std::vector<char> const *vec, WebClientHandler *handler)
{
	if (vec->empty()) return;
	char const *begin = &vec->at(0);
	char const *end = begin + vec->size();
	parse_http_header(begin, end, &pv->response);
	if (handler) {
		handler->checkHeader(this);
	}
}

void WebClient::append(char const *ptr, size_t len, std::vector<char> *out, WebClientHandler *handler)
{
	size_t offset = out->size();
	out->insert(out->end(), ptr, ptr + len);

	if (pv->crlf_state < 0) {
		// nop
	} else {
		for (size_t i = 0; i < len; i++) {
			int c = ptr[i] & 0xff;
			if (c == '\r') {
				pv->crlf_state |= 1;
			} else if (c == '\n') {
				pv->crlf_state |= 1;
				pv->crlf_state++;
			} else {
				pv->crlf_state = 0;
			}
			if (pv->crlf_state == 4) {
				pv->content_offset = offset + i + 1;
				on_end_header(out, handler);
				pv->crlf_state = -1;
				break;
			}
		}
	}
	if (handler && pv->content_offset > 0) {
		offset = out->size();
		if (offset > pv->content_offset) {
			size_t len = offset - pv->content_offset;
			char const *ptr = &out->at(pv->content_offset);
			handler->checkContent(ptr, len);
		}
	}
}

static char *stristr(char *str1, char const *str2)
{
	size_t len1 = strlen(str1);
	size_t len2 = strlen(str2);
	for (size_t i = 0; i + len2 <= len1; i++) {
		if (strnicmp(str1 + i, str2, len2) == 0) {
			return str1 + i;
		}
	}
	return nullptr;
}

class ResponseHeader {
public:
	size_t pos = 0;
	std::vector<char> line;
	int content_length = -1;
	bool connection_keep_alive = false;
	bool connection_close = false;
	int lf = 0;
	enum State {
		Header,
		Content,
	};
	State state = Header;
	void put(int c)
	{
		pos++;
		if (state == Header) {
			if (c== '\r' || c == '\n') {
				if (!line.empty()) {
					line.push_back(0);
					char *begin = &line[0];
					char *p = strchr(begin, ':');
					if (p && *p == ':') {
						*p++ = 0;
						auto IS = [&](char const *name){ return stricmp(begin, name) == 0; };
						if (IS("content-length")) {
							content_length = strtol(p, 0, 10);
						} else if (IS("connection")) {
							if (stristr(p, "keep-alive")) {
								connection_keep_alive = true;
							} else if (stristr(p, "close")) {
								connection_close = true;
							}
						}
					}
					line.clear();
				}
				if (c== '\r') {
					return;
				}
				if (c == '\n') {
					lf++;
					if (lf == 2) {
						state = Content;
					}
					return;
				}
			}
			lf = 0;
			line.push_back(c);
		}
	}
};

void WebClient::receive_(RequestOption const &opt, std::function<int(char *, int)> rcv, std::vector<char> *out)
{
	char buf[4096];
	size_t pos = 0;
	ResponseHeader rh;
	while (1) {
		int n;
		if (rh.state == ResponseHeader::Content && rh.content_length >= 0) {
			n = rh.pos + rh.content_length - pos;
			if (n > (int)sizeof(buf)) {
				n = sizeof(buf);
			}
			if (n < 1) break;
		} else {
			n = sizeof(buf);
		}
		n = rcv(buf, n);
		if (n < 1) break;
		append(buf, n, out, opt.handler);
		pos += n;
		if (rh.state == ResponseHeader::Header) {
			for (int i = 0; i < n; i++) {
				rh.put(buf[i]);
				if (rh.state == ResponseHeader::Content) {
					pv->keep_alive = rh.connection_keep_alive && !rh.connection_close;
					break;
				}
			}
		}
	}
}

bool WebClient::http_get(URL const &uri, Post const *post, RequestOption const &opt, std::vector<char> *out)
{
	clear_error();
	out->clear();

	std::string hostname = uri.host();

	pv->keep_alive = opt.keep_alive && hostname == pv->last_host_name;
	if (!pv->keep_alive) close();

	if (pv->sock == INVALID_SOCKET) {
		struct hostent *servhost;
		struct sockaddr_in server;

		servhost = gethostbyname(hostname.c_str());
		if (!servhost) {
			throw Error("gethostbyname failed.");
		}

		memset((char *)&server, 0, sizeof(server));
		server.sin_family = AF_INET;

		memcpy((char *)&server.sin_addr, servhost->h_addr, servhost->h_length);

		server.sin_port = htons(get_port(&uri, "http", "tcp"));

		pv->sock = socket(AF_INET, SOCK_STREAM, 0);
		if (pv->sock == INVALID_SOCKET) {
			throw Error("socket failed.");
		}

		if (connect(pv->sock, (struct sockaddr*) &server, sizeof(server)) == SOCKET_ERROR) {
			throw Error("connect failed.");
		}
	}
	pv->last_host_name = hostname;

	set_default_header(uri, post, opt);

	std::string request = make_http_request(uri, post);

	send_(pv->sock, request.c_str(), (int)request.size());
	if (post && !post->data.empty()) {
		send_(pv->sock, (char const *)&post->data[0], (int)post->data.size());
	}

	pv->crlf_state = 0;
	pv->content_offset = 0;

	receive_(opt, [&](char *ptr, int len){
		return recv(pv->sock, ptr, len, 0);
	}, out);

	if (!pv->keep_alive) close();

	return true;
}

bool WebClient::https_get(const URL &uri, Post const *post, RequestOption const &opt, std::vector<char> *out)
{
#if USE_OPENSSL
	auto sslctx = [&](){ return pv->webcx->pv->ctx; };

	if (!pv->webcx || !pv->webcx->pv->ctx) {
		output_debug_string("SSL context is null.\n");
		return false;
	}

	clear_error();
	out->clear();

	auto get_ssl_error = []()->std::string{
			char tmp[1000];
	unsigned long e = ERR_get_error();
	ERR_error_string_n(e, tmp, sizeof(tmp));
	return tmp;
};

std::string hostname = uri.host();

pv->keep_alive = opt.keep_alive && hostname == pv->last_host_name;
if (!pv->keep_alive) close();

if (pv->sock == INVALID_SOCKET || !pv->ssl) {
	int ret;
	struct hostent *servhost;
	struct sockaddr_in server;

	servhost = gethostbyname(uri.host().c_str());
	if (!servhost) {
		throw Error("gethostbyname failed.");
	}

	memset((char *)&server, 0, sizeof(server));
	server.sin_family = AF_INET;

	memcpy((char *)&server.sin_addr, servhost->h_addr, servhost->h_length);

	server.sin_port = htons(get_port(&uri, "https", "tcp"));

	pv->sock = socket(AF_INET, SOCK_STREAM, 0);
	if (pv->sock == INVALID_SOCKET) {
		throw Error("socket failed.");
	}

	if (connect(pv->sock, (struct sockaddr*) &server, sizeof(server)) == SOCKET_ERROR) {
		throw Error("connect failed.");
	}

	pv->ssl = SSL_new(sslctx());
	if (!pv->ssl) {
		throw Error(get_ssl_error());
	}

	SSL_set_options(pv->ssl, SSL_OP_NO_SSLv2);
	SSL_set_options(pv->ssl, SSL_OP_NO_SSLv3);

	ret = SSL_set_fd(pv->ssl, pv->sock);
	if (ret == 0) {
		throw Error(get_ssl_error());
	}

	RAND_poll();
	while (RAND_status() == 0) {
		unsigned short rand_ret = rand() % 65536;
		RAND_seed(&rand_ret, sizeof(rand_ret));
	}

	ret = SSL_connect(pv->ssl);
	if (ret != 1) {
		throw Error(get_ssl_error());
	}

	std::string cipher = SSL_get_cipher(pv->ssl);
	cipher += '\n';
	output_debug_string(cipher.c_str());

	std::string version = SSL_get_cipher_version(pv->ssl);
	version += '\n';
	output_debug_string(version.c_str());

	X509 *x509 = SSL_get_peer_certificate(pv->ssl);
	if (x509) {
		std::string fingerprint;
		for (int i = 0; i < SHA_DIGEST_LENGTH; i++) {
			if (i > 0) {
				fingerprint += ':';
			}
			char tmp[10];
			sprintf(tmp, "%02X", x509->sha1_hash[i]);
			fingerprint += tmp;
		}
		fingerprint += '\n';
		output_debug_string(fingerprint.c_str());


		long l = SSL_get_verify_result(pv->ssl);
		if (l == X509_V_OK) {
			// ok
		} else {
			// wrong
			std::string err = X509_verify_cert_error_string(l);
			err += '\n';
			output_debug_string(err.c_str());
		}

		std::vector<std::string> vec;

		auto GETSTRINGS = [](X509_NAME *x509name, std::vector<std::string> *out){
			out->clear();
			if (x509name) {
				int n = X509_NAME_entry_count(x509name);
				for (int i = 0; i < n; i++) {
					X509_NAME_ENTRY *entry = X509_NAME_get_entry(x509name, i);
					ASN1_STRING *asn1str = X509_NAME_ENTRY_get_data(entry);
					int asn1len = ASN1_STRING_length(asn1str);
					unsigned char *p = ASN1_STRING_data(asn1str);
					std::string str((char const *)p, asn1len);
					out->push_back(str);
				}
			}
		};

		X509_NAME *subject = X509_get_subject_name(x509);
		GETSTRINGS(subject, &vec);
		output_debug_string("--- subject ---\n");
		output_debug_strings(vec);

		X509_NAME *issuer = X509_get_issuer_name(x509);
		GETSTRINGS(issuer, &vec);
		output_debug_string("--- issuer ---\n");
		output_debug_strings(vec);

		ASN1_TIME *not_before = X509_get_notBefore(x509);
		ASN1_TIME *not_after  = X509_get_notAfter(x509);
		(void)not_before;
		(void)not_after;

		X509_free(x509);
	} else {
		// wrong
	}
}
pv->last_host_name = hostname;

set_default_header(uri, post, opt);

std::string request = make_http_request(uri, post);

auto SEND = [&](SSL *ssl, char const *ptr, int len){
	while (len > 0) {
		int n = SSL_write(ssl, ptr, len);
		if (n < 1 || n > len) {
			throw WebClient::Error(get_ssl_error());
		}
		ptr += n;
		len -= n;
	}
};

SEND(pv->ssl, request.c_str(), (int)request.size());
if (post && !post->data.empty()) {
	SEND(pv->ssl, (char const *)&post->data[0], (int)post->data.size());
}

pv->crlf_state = 0;
pv->content_offset = 0;

receive_(opt, [&](char *ptr, int len){
	return SSL_read(pv->ssl, ptr, len);
}, out);

if (!pv->keep_alive) close();

return true;
#endif
return false;
}

void WebClient::get(URL const &uri, Post const *post, Response *out, WebClientHandler *handler)
{
	*out = Response();
	try {
		if (!pv->webcx->pv) {
			throw Error("WebContext is null.");
		}
		RequestOption opt;
		opt.keep_alive = pv->webcx->pv->use_keep_alive;
		opt.handler = handler;
		std::vector<char> res;
		if (uri.isssl()) {
#if USE_OPENSSL
			https_get(uri, post, opt, &res);
#endif
		} else {
			http_get(uri, post, opt, &res);
		}
		if (!res.empty()) {
			char const *begin = &res[0];
			char const *end = begin + res.size();
			char const *ptr = begin + pv->content_offset;
			if (ptr < end) {
				out->content.assign(ptr, end);
			}
		}
		return;
	} catch (Error const &e) {
		if (handler) {
			pv->error = e;
		}
	}
	*out = Response();
}

void WebClient::parse_header(std::vector<std::string> const *header, WebClient::Response *res)
{
	if (header->size() > 0) {
		std::string const &line = header->at(0);
		char const *begin = line.c_str();
		char const *end = begin + line.size();
		if (line.size() > 5 && strncmp(line.c_str(), "HTTP/", 5) == 0) {
			int state = 0;
			res->version.hi = res->version.lo = res->code = 0;
			char const *ptr = begin + 5;
			while (1) {
				int c = 0;
				if (ptr < end) {
					c = *ptr & 0xff;
				}
				switch (state) {
				case 0:
					if (isdigit(c)) {
						res->version.hi = res->version.hi * 10 + (c - '0');
					} else if (c == '.') {
						state = 1;
					} else {
						state = -1;
					}
					break;
				case 1:
					if (isdigit(c)) {
						res->version.lo = res->version.lo * 10 + (c - '0');
					} else if (isspace(c)) {
						state = 2;
					} else {
						state = -1;
					}
					break;
				case 2:
					if (isspace(c)) {
						if (res->code != 0) {
							state = -1;
						}
					} else if (isdigit(c)) {
						res->code = res->code * 10 + (c - '0');
					} else {
						state = -1;
					}
					break;
				default:
					state = -1;
					break;
				}
				if (c == 0 || state < 0) {
					break;
				}
				ptr++;
			}
		}
	}
}

std::string WebClient::header_value(std::vector<std::string> const *header, std::string const &name)
{
	for (size_t i = 1; i < header->size(); i++) {
		std::string const &line = header->at(i);
		char const *begin = line.c_str();
		char const *end = begin + line.size();
		char const *colon = strchr(begin, ':');
		if (colon) {
			if (strnicmp(begin, name.c_str(), name.size()) == 0) {
				char const *ptr = colon + 1;
				while (ptr < end && isspace(*ptr & 0xff)) ptr++;
				return std::string(ptr, end);
			}
		}
	}
	return std::string();
}

std::string WebClient::header_value(std::string const &name) const
{
	return header_value(&pv->response.header, name);
}

std::string WebClient::content_type() const
{
	std::string s = header_value("Content-Type");
	char const *begin = s.c_str();
	char const *end = begin + s.size();
	char const *ptr = begin;
	while (ptr < end) {
		int c = *ptr & 0xff;
		if (c == ';' || c < 0x21) break;
		ptr++;
	}
	if (ptr < end) return std::string(begin, ptr);
	return s;
}

size_t WebClient::content_length() const
{
	return pv->response.content.size();
}

char const *WebClient::content_data() const
{
	if (pv->response.content.empty()) return "";
	return &pv->response.content[0];
}

int WebClient::get(URL const &uri, WebClientHandler *handler)
{
	get(uri, nullptr, &pv->response, handler);
	return pv->response.code;
}

int WebClient::post(URL const &uri, Post const *post, WebClientHandler *handler)
{
	get(uri, post, &pv->response, handler);
	return pv->response.code;
}

void WebClient::close()
{
#if USE_OPENSSL
	if (pv->ssl) {
		SSL_shutdown(pv->ssl);
		SSL_free(pv->ssl);
		pv->ssl = nullptr;
	}
#endif
	if (pv->sock != INVALID_SOCKET) {
		closesocket(pv->sock);
		pv->sock = INVALID_SOCKET;
	}
}

void WebClient::add_header(std::string const &text)
{
	pv->request_header.push_back(text);
}

WebClient::Response const &WebClient::response() const
{
	return pv->response;
}

static void write(std::vector<char> *out, char const *begin, char const *end)
{
	out->insert(out->end(), begin, end);
}

static void write(std::vector<char> *out, char const *p, int n = -1)
{
	if (n < 0) n = strlen(p);
	write(out, p, p + n);
}

static void write(std::vector<char> *out, std::string const &str)
{
	write(out, str.c_str(), (int)str.size());
}

void WebClient::make_application_www_form_urlencoded(char const *begin, char const *end, WebClient::Post *out)
{
	*out = WebClient::Post();
	out->content_type = CT_APPLICATION_X_WWW_FORM_URLENCODED;
	write(&out->data, begin, end - begin);
}

void WebClient::make_multipart_form_data(std::vector<Part> const &parts, WebClient::Post *out, std::string const &boundary)
{
	*out = WebClient::Post();
	out->content_type = CT_MULTIPART_FORM_DATA;
	out->boundary = boundary;

	for (Part const &part : parts) {
		write(&out->data, "--");
		write(&out->data, out->boundary);
		write(&out->data, "\r\n");
		if (!part.content_disposition.type.empty()) {
			ContentDisposition const &cd = part.content_disposition;
			std::string s;
			s = "Content-Disposition: ";
			s += cd.type;
			auto Add = [&s](std::string const &name, std::string const &value){
				if (!value.empty()) {
					s += "; " + name + "=\"";
					s += value;
					s += '\"';
				}
			};
			Add("name", cd.name);
			Add("filename", cd.filename);
			write(&out->data, s);
			write(&out->data, "\r\n");
		}
		if (!part.content_type.empty()) {
			write(&out->data, "Content-Type: " + part.content_type + "\r\n");
		}
		if (!part.content_transfer_encoding.empty()) {
			write(&out->data, "Content-Transfer-Encoding: " + part.content_transfer_encoding + "\r\n");
		}
		write(&out->data, "\r\n");
		write(&out->data, part.data, part.size);
		write(&out->data, "\r\n");
	}

	write(&out->data, "--");
	write(&out->data, out->boundary);
	write(&out->data, "--\r\n");
}

void WebClient::make_multipart_form_data(char const *data, size_t size, WebClient::Post *out, std::string const &boundary)
{
	Part part;
	part.data = data;
	part.size = size;
	std::vector<Part> parts;
	parts.push_back(part);
	make_multipart_form_data(parts, out, boundary);
}


//

WebContext::WebContext()
{
	pv = new Private();
#if USE_OPENSSL
	SSL_load_error_strings();
	SSL_library_init();
	pv->ctx = SSL_CTX_new(SSLv23_client_method());
#endif
}

WebContext::~WebContext()
{
#if USE_OPENSSL
	SSL_CTX_free(pv->ctx);
#endif
	delete pv;
}

void WebContext::set_keep_alive_enabled(bool f)
{
	pv->use_keep_alive = f;
}

bool WebContext::load_cacert(char const *path)
{
#if USE_OPENSSL
	int r = SSL_CTX_load_verify_locations(pv->ctx, path, 0);
	return r == 1;
#endif
	return false;
}

