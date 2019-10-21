#include <unistd.h>
#include <sys/socket.h>
#include <netinet/ip.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <string.h>

#include "nic.h"

using namespace std;

int nic_t::start_client_socket(string ip_address, uint16_t port) {
	int sockfd = socket(AF_INET, SOCK_STREAM, 0);
	if (sockfd < 0) {
		return -1;
	}
	struct in_addr network_order_address;
	int ip_conversion_retval = inet_aton(ip_address.c_str(), &network_order_address);
	struct sockaddr_in addr;
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);
	addr.sin_addr = network_order_address;
	int connect_retval = connect(sockfd, (struct sockaddr*)&addr, sizeof(addr));
	if (connect_retval < 0) {
		close(sockfd);
		return -1;
	}
	return sockfd;
}

nic_t::nic_t() {
	string ip_address = "127.0.0.1";
	uint16_t port = 9000;
	// TODO: The backend here can also be implemented with a file
	_nic_fd = start_client_socket(ip_address, port);
}

void nic_t::store_uint64(reg_t data) {
	if (_nic_fd < 0) {
		return; // Do nothing if the file descriptor is invalid
	}
	// TODO: There is no error checking on these read and write calls
	write(_nic_fd, &data, sizeof(data));
}

reg_t nic_t::load_uint64() {
	if (_nic_fd < 0) {
		return 0; // Load a 0 into the dest register if the
		          // file descriptor is invalid.
	}
	reg_t to_return;
	read(_nic_fd, &to_return, sizeof(to_return));
	return to_return;
}