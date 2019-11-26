/*
  * This program is free software: you can redistribute it and/or modify
  * it under the terms of the GNU General Public License as published by
  * the Free Software Foundation, either version 3 of the License, or
  * any later version.
  *
  * This program is distributed in the hope that it will be useful,
  * but WITHOUT ANY WARRANTY; without even the implied warranty of
  * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
  * GNU General Public License for more details.
  *
  * You should have received a copy of the GNU General Public License
  * along with this program.  If not, see <http://www.gnu.org/licenses/>.
  *
  * Additional permission under GNU GPL version 3 section 7
  *
  * If you modify this Program, or any covered work, by linking or combining
  * it with OpenSSL (or a modified version of that library), containing parts
  * covered by the terms of OpenSSL License and SSLeay License, the licensors
  * of this Program grant you additional permission to convey the resulting work.
  *
  */

#include "socket.hpp"
#include "jpsock.hpp"
#include "xmrstak/jconf.hpp"
#include "xmrstak/misc/console.hpp"
#include "xmrstak/misc/executor.hpp"
#include "xmrstak/misc/motd.hpp"
#include "xmrstak/version.hpp"

#ifndef CONF_NO_TLS
#include <openssl/err.h>
#include <openssl/opensslconf.h>
#include <openssl/ssl.h>
#include <type_traits>

#ifndef OPENSSL_THREADS
#error OpenSSL was compiled without thread support
#endif
#endif

class callback_holder
{
public:
	bool set_socket_error(const char*, size_t len) { return true; }
	bool set_socket_error(const char*) { return true; }
	const char* get_tls_fp() { return ""; }
	const char* get_pool_addr() { return "";} ;
};

plain_socket::plain_socket(jpsock* err_callback) :
	pCallback(err_callback)
{
	hSocket = INVALID_SOCKET;
	pSockAddr = nullptr;
}

bool plain_socket::set_hostname(const char* sAddr)
{
	char sAddrMb[256];
	char *sTmp, *sPort;

	sock_closed = false;
	size_t ln = strlen(sAddr);
	if(ln >= sizeof(sAddrMb))
		return pCallback->set_socket_error("CONNECT error: Pool address overflow.");

	memcpy(sAddrMb, sAddr, ln);
	sAddrMb[ln] = '\0';

	if((sTmp = strstr(sAddrMb, "//")) != nullptr)
	{
		sTmp += 2;
		memmove(sAddrMb, sTmp, strlen(sTmp) + 1);
	}

	if((sPort = strchr(sAddrMb, ':')) == nullptr)
		return pCallback->set_socket_error("CONNECT error: Pool port number not specified, please use format <hostname>:<port>.");

	sPort[0] = '\0';
	sPort++;

	addrinfo hints = {0};
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = IPPROTO_TCP;

	pAddrRoot = nullptr;
	int err;
	if((err = getaddrinfo(sAddrMb, sPort, &hints, &pAddrRoot)) != 0)
		return pCallback->set_socket_error_strerr("CONNECT error: GetAddrInfo: ", err);

	addrinfo* ptr = pAddrRoot;
	std::vector<addrinfo*> ipv4;
	std::vector<addrinfo*> ipv6;

	while(ptr != nullptr)
	{
		if(ptr->ai_family == AF_INET)
			ipv4.push_back(ptr);
		if(ptr->ai_family == AF_INET6)
			ipv6.push_back(ptr);
		ptr = ptr->ai_next;
	}

	if(ipv4.empty() && ipv6.empty())
	{
		freeaddrinfo(pAddrRoot);
		pAddrRoot = nullptr;
		return pCallback->set_socket_error("CONNECT error: I found some DNS records but no IPv4 or IPv6 addresses.");
	}
	else if(!ipv4.empty() && ipv6.empty())
		pSockAddr = ipv4[rand() % ipv4.size()];
	else if(ipv4.empty() && !ipv6.empty())
		pSockAddr = ipv6[rand() % ipv6.size()];
	else if(!ipv4.empty() && !ipv6.empty())
	{
		if(jconf::inst()->PreferIpv4())
			pSockAddr = ipv4[rand() % ipv4.size()];
		else
			pSockAddr = ipv6[rand() % ipv6.size()];
	}

	hSocket = socket(pSockAddr->ai_family, pSockAddr->ai_socktype, pSockAddr->ai_protocol);

	if(hSocket == INVALID_SOCKET)
	{
		freeaddrinfo(pAddrRoot);
		pAddrRoot = nullptr;
		return pCallback->set_socket_error_strerr("CONNECT error: Socket creation failed ");
	}

	int flag = 1;
	/* If it fails, it fails, we won't loose too much sleep over it */
	setsockopt(hSocket, IPPROTO_TCP, TCP_NODELAY, (char*)&flag, sizeof(int));

	return true;
}

bool plain_socket::connect()
{
	sock_closed = false;
	int ret = ::connect(hSocket, pSockAddr->ai_addr, (int)pSockAddr->ai_addrlen);

	freeaddrinfo(pAddrRoot);
	pAddrRoot = nullptr;

	if(ret != 0)
		return pCallback->set_socket_error_strerr("CONNECT error: ");
	else
		return true;
}

int plain_socket::recv(char* buf, unsigned int len)
{
	if(sock_closed)
		return 0;

	int ret = ::recv(hSocket, buf, len, 0);

	if(ret == 0)
		pCallback->set_socket_error("RECEIVE error: socket closed");
	if(ret == SOCKET_ERROR || ret < 0)
		pCallback->set_socket_error_strerr("RECEIVE error: ");

	return ret;
}

bool plain_socket::send(const char* buf)
{
	size_t pos = 0;
	size_t slen = strlen(buf);
	while(pos != slen)
	{
		int ret = ::send(hSocket, buf + pos, slen - pos, 0);
		if(ret == SOCKET_ERROR)
		{
			pCallback->set_socket_error_strerr("SEND error: ");
			return false;
		}
		else
			pos += ret;
	}

	return true;
}

void plain_socket::close(bool free)
{
	if(hSocket != INVALID_SOCKET)
	{
		sock_closed = true;
		sock_close(hSocket);
		hSocket = INVALID_SOCKET;
	}
}

#ifndef CONF_NO_TLS
template <typename T>
void tls_socket_t<T>::print_error()
{
	BIO* err_bio = BIO_new(BIO_s_mem());
	ERR_print_errors(err_bio);

	char* buf = nullptr;
	size_t len = BIO_get_mem_data(err_bio, &buf);

	if(buf == nullptr)
	{
		if(jconf::inst()->TlsSecureAlgos())
			pCallback->set_socket_error("Unknown TLS error. Secure TLS maybe unsupported, try setting tls_secure_algo to false.");
		else
			pCallback->set_socket_error("Unknown TLS error. You might be trying to connect to a non-TLS port.");
	}
	else
		pCallback->set_socket_error(buf, len);

	BIO_free(err_bio);
}

template <typename T>
void tls_socket_t<T>::init_ctx()
{
	const SSL_METHOD* method = SSLv23_method();

	if(method == nullptr)
		return;

	ctx = SSL_CTX_new(method);
	if(ctx == nullptr)
		return;

	if(jconf::inst()->TlsSecureAlgos())
	{
		SSL_CTX_set_options(ctx, SSL_OP_NO_SSLv2 | SSL_OP_NO_SSLv3 | SSL_OP_NO_TLSv1);
	}
}

template <typename T>
bool tls_socket_t<T>::set_hostname(const char* sAddr)
{
	sock_closed = false;
	if(ctx == nullptr)
	{
		init_ctx();
		if(ctx == nullptr)
		{
			print_error();
			return false;
		}
	}

	if((bio = BIO_new_ssl_connect(ctx)) == nullptr)
	{
		print_error();
		return false;
	}

	int flag = 1;
	/* If it fails, it fails, we won't loose too much sleep over it */
	setsockopt(BIO_get_fd(bio, nullptr), IPPROTO_TCP, TCP_NODELAY, (char*)&flag, sizeof(int));

	if(BIO_set_conn_hostname(bio, sAddr) != 1)
	{
		print_error();
		return false;
	}

	BIO_get_ssl(bio, &ssl);
	if(ssl == nullptr)
	{
		print_error();
		return false;
	}

	if(jconf::inst()->TlsSecureAlgos())
	{
		if(SSL_set_cipher_list(ssl, "HIGH:!aNULL:!PSK:!SRP:!MD5:!RC4:!SHA1") != 1)
		{
			print_error();
			return false;
		}
	}

	return true;
}

template <typename T>
bool tls_socket_t<T>::connect()
{
	sock_closed = false;
	if(BIO_do_connect(bio) != 1)
	{
		print_error();
		return false;
	}

	if(BIO_do_handshake(bio) != 1)
	{
		print_error();
		return false;
	}

	/* Step 1: verify a server certificate was presented during the negotiation */
	X509* cert = SSL_get_peer_certificate(ssl);
	if(cert == nullptr)
	{
		print_error();
		return false;
	}

	const EVP_MD* digest;
	unsigned char md[EVP_MAX_MD_SIZE];
	unsigned int dlen;

	digest = EVP_get_digestbyname("sha256");
	if(digest == nullptr)
	{
		print_error();
		return false;
	}

	if(X509_digest(cert, digest, md, &dlen) != 1)
	{
		X509_free(cert);
		print_error();
		return false;
	}

	//Base64 encode digest
	BIO *bmem, *b64;
	b64 = BIO_new(BIO_f_base64());
	bmem = BIO_new(BIO_s_mem());

	BIO_puts(bmem, "SHA256:");
	b64 = BIO_push(b64, bmem);
	BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);
	BIO_write(b64, md, dlen);
	BIO_flush(b64);

	const char* conf_md = pCallback->get_tls_fp();
	char* b64_md = nullptr;
	size_t b64_len = BIO_get_mem_data(bmem, &b64_md);

	// disable fingerprint check for motd server
	if(!std::is_same<T, callback_holder>::value)
	{
		if(strlen(conf_md) == 0)
		{
			printer::inst()->print_msg(L1, "TLS fingerprint [%s] %.*s", pCallback->get_pool_addr(), (int)b64_len, b64_md);
		}
		else if(strncmp(b64_md, conf_md, b64_len) != 0)
		{
			printer::inst()->print_msg(L0, "FINGERPRINT FAILED CHECK [%s] %.*s was given, %s was configured",
				pCallback->get_pool_addr(), (int)b64_len, b64_md, conf_md);

			pCallback->set_socket_error("FINGERPRINT FAILED CHECK");
			BIO_free_all(b64);
			X509_free(cert);
			return false;
		}
	}

	BIO_free_all(b64);

	X509_free(cert);
	return true;
}

template <typename T>
int tls_socket_t<T>::recv(char* buf, unsigned int len)
{
	if(sock_closed)
		return 0;

	int ret = BIO_read(bio, buf, len);

	if(ret == 0)
		pCallback->set_socket_error("RECEIVE error: socket closed");
	if(ret < 0)
		print_error();

	return ret;
}

template <typename T>
bool tls_socket_t<T>::send(const char* buf)
{
	return BIO_puts(bio, buf) > 0;
}

template <typename T>
void tls_socket_t<T>::close(bool free)
{
	if(bio == nullptr || ssl == nullptr)
		return;

	sock_closed = true;
	if(!free)
	{
		sock_close(BIO_get_fd(bio, nullptr));
	}
	else
	{
		BIO_free_all(bio);
		ssl = nullptr;
		bio = nullptr;
	}
}

std::string entry_vector_to_json(const std::string category, const std::vector<xmrstak::system_entry> vec)
{
	std::string json;
	json += "\"" + category + "\" : [";
	int count = 0;
	for( auto const e : vec)
	{
		if(count++ != 0)
			json += ",";
		json += "{";
		json += "\"make\" : \"" + e.make+ "\", ";
		json += "\"threads\" : " + std::to_string(e.num_threads);
		json += "}";
	}
	json += "]";

	return json;
}

inline void get_motd()
{
	callback_holder ch;
	tls_socket_t<callback_holder> socket(&ch);
	if(!socket.set_hostname("donate.xmr-stak.net:14441"))
	{
		printer::inst()->print_msg(LDEBUG, "Motd server set hostname error!\n");
		socket.close(true);
		return;
	}
	if(!socket.connect())
	{
		printer::inst()->print_msg(LDEBUG, "Connecting to motd server failed!\n");
		socket.close(true);
		return;
	}

	std::string json;

	if(!xmrstak::params::inst().cpu_devices.empty())
		json += entry_vector_to_json("cpu", xmrstak::params::inst().cpu_devices);

	if(!xmrstak::params::inst().cuda_devices.empty())
	{
		if(!json.empty())
			json += ",";
		json += entry_vector_to_json("cuda", xmrstak::params::inst().cuda_devices);
	}

	if(!xmrstak::params::inst().opencl_devices.empty())
	{
		if(!json.empty())
			json += ",";
		json += entry_vector_to_json("opencl", xmrstak::params::inst().opencl_devices);
	}

	const std::string user_agent =
		std::string("{ \"version\" : \"") + get_version_str() + "\", " +
		std::string("\"algo\" : \"") + ::jconf::inst()->GetCurrentCoinSelection().GetDescription().GetMiningAlgo().Name() + "\", " +
		std::string("\"system\" : {") + json + "}}\n";

	//for debug
	//printer::inst()->print_msg(LDEBUG, "%s",user_agent.c_str());


	socket.send(user_agent.data());

	char buffer[2048];
	std::string motd;
	while(true)
	{
		int recv = socket.recv(buffer, sizeof(buffer));
		if(recv > 0)
		{
			buffer[recv] = 0;
			motd.append(buffer, recv + 1);
			continue;
		}
		break;
	}
	socket.close(true);

	if(motd.size() > 0)
		xmrstak::motd::inst().set_message(std::move(motd));
	else
		printer::inst()->print_msg(LDEBUG, "Error receiving motd!");
}

void update_motd(bool force)
{
	static size_t timestamp = 0u;
	if(force || timestamp == 0u || (get_timestamp() - timestamp > 60*60))
	{
		std::thread motd_thd(&get_motd);
		motd_thd.detach();
		timestamp = get_timestamp();
	}
}
template class tls_socket_t<callback_holder>;
template class tls_socket_t<jpsock>;
#else
void update_motd(bool)
{

}
#endif
