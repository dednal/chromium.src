// Copyright 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>

#include <map>
#include <string>

#include "echo_server.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "nacl_io/kernel_intercept.h"
#include "nacl_io/kernel_proxy.h"
#include "nacl_io/ossocket.h"
#include "nacl_io/ostypes.h"
#include "ppapi/cpp/message_loop.h"
#include "ppapi_simple/ps.h"

#ifdef PROVIDES_SOCKET_API

using namespace nacl_io;
using namespace sdk_util;

#define LOCAL_HOST 0x7F000001
#define PORT1 4006
#define PORT2 4007
#define ANY_PORT 0

namespace {

void IP4ToSockAddr(uint32_t ip, uint16_t port, struct sockaddr_in* addr) {
  memset(addr, 0, sizeof(*addr));

  addr->sin_family = AF_INET;
  addr->sin_port = htons(port);
  addr->sin_addr.s_addr = htonl(ip);
}

void SetNonBlocking(int sock) {
  int flags = fcntl(sock, F_GETFL);
  ASSERT_NE(-1, flags);
  flags |= O_NONBLOCK;
  ASSERT_EQ(0, fcntl(sock, F_SETFL, flags));
  ASSERT_EQ(flags, fcntl(sock, F_GETFL));
}

class SocketTest : public ::testing::Test {
 public:
  SocketTest() : sock1_(-1), sock2_(-1) {}

  void TearDown() {
    if (sock1_ != -1)
      EXPECT_EQ(0, close(sock1_));
    if (sock2_ != -1)
      EXPECT_EQ(0, close(sock2_));
  }

  int Bind(int fd, uint32_t ip, uint16_t port) {
    sockaddr_in addr;
    socklen_t addrlen = sizeof(addr);

    IP4ToSockAddr(ip, port, &addr);
    int err = bind(fd, (sockaddr*)&addr, addrlen);

    if (err == -1)
      return errno;
    return 0;
  }

 protected:
  int sock1_;
  int sock2_;
};

class SocketTestUDP : public SocketTest {
 public:
  SocketTestUDP() {}

  void SetUp() {
    sock1_ = socket(AF_INET, SOCK_DGRAM, 0);
    sock2_ = socket(AF_INET, SOCK_DGRAM, 0);

    EXPECT_GT(sock1_, -1);
    EXPECT_GT(sock2_, -1);
  }
};

class SocketTestTCP : public SocketTest {
 public:
  SocketTestTCP() {}

  void SetUp() {
    sock1_ = socket(AF_INET, SOCK_STREAM, 0);
    sock2_ = socket(AF_INET, SOCK_STREAM, 0);

    EXPECT_GT(sock1_, -1);
    EXPECT_GT(sock2_, -1);
  }
};

class SocketTestWithServer : public ::testing::Test {
 public:
  SocketTestWithServer() : instance_(PSGetInstanceId()) {
    pthread_mutex_init(&ready_lock_, NULL);
    pthread_cond_init(&ready_cond_, NULL);
  }

  void ServerThreadMain() {
    loop_.AttachToCurrentThread();
    pp::Instance instance(PSGetInstanceId());
    EchoServer server(&instance, PORT1, ServerLog, &ready_cond_, &ready_lock_);
    loop_.Run();
  }

  static void* ServerThreadMainStatic(void* arg) {
    SocketTestWithServer* test = (SocketTestWithServer*)arg;
    test->ServerThreadMain();
    return NULL;
  }

  void SetUp() {
    loop_ = pp::MessageLoop(&instance_);
    pthread_mutex_lock(&ready_lock_);

    // Start an echo server on a background thread.
    pthread_create(&server_thread_, NULL, ServerThreadMainStatic, this);

    // Wait for thread to signal that it is ready to accept connections.
    pthread_cond_wait(&ready_cond_, &ready_lock_);
    pthread_mutex_unlock(&ready_lock_);

    sock_ = socket(AF_INET, SOCK_STREAM, 0);
    EXPECT_GT(sock_, -1);
  }

  void TearDown() {
    // Stop the echo server and the background thread it runs on
    loop_.PostQuit(true);
    pthread_join(server_thread_, NULL);
    ASSERT_EQ(0, close(sock_));
  }

  static void ServerLog(const char* msg) {
    // Uncomment to see logs of echo server on stdout
    //printf("server: %s\n", msg);
  }

 protected:
  int sock_;
  pp::MessageLoop loop_;
  pp::Instance instance_;
  pthread_cond_t ready_cond_;
  pthread_mutex_t ready_lock_;
  pthread_t server_thread_;
};

}  // namespace

TEST(SocketTestSimple, Socket) {
  EXPECT_EQ(-1, socket(AF_UNIX, SOCK_STREAM, 0));
  EXPECT_EQ(errno, EAFNOSUPPORT);
  EXPECT_EQ(-1, socket(AF_INET, SOCK_RAW, 0));
  EXPECT_EQ(errno, EPROTONOSUPPORT);

  int sock1_ = socket(AF_INET, SOCK_DGRAM, 0);
  EXPECT_NE(-1, sock1_);

  int sock2_ = socket(AF_INET6, SOCK_DGRAM, 0);
  EXPECT_NE(-1, sock2_);

  int sock3 = socket(AF_INET, SOCK_STREAM, 0);
  EXPECT_NE(-1, sock3);

  int sock4 = socket(AF_INET6, SOCK_STREAM, 0);
  EXPECT_NE(-1, sock4);

  close(sock1_);
  close(sock2_);
  close(sock3);
  close(sock4);
}

TEST_F(SocketTestUDP, Bind) {
  // Bind away.
  EXPECT_EQ(0, Bind(sock1_, LOCAL_HOST, PORT1));

  // Invalid to rebind a socket.
  EXPECT_EQ(EINVAL, Bind(sock1_, LOCAL_HOST, PORT1));

  // Addr in use.
  EXPECT_EQ(EADDRINUSE, Bind(sock2_, LOCAL_HOST, PORT1));

  // Bind with a wildcard.
  EXPECT_EQ(0, Bind(sock2_, LOCAL_HOST, ANY_PORT));

  // Invalid to rebind after wildcard
  EXPECT_EQ(EINVAL, Bind(sock2_, LOCAL_HOST, PORT1));
}

TEST_F(SocketTestUDP, SendRcv) {
  char outbuf[256];
  char inbuf[512];

  memset(outbuf, 1, sizeof(outbuf));
  memset(inbuf, 0, sizeof(inbuf));

  EXPECT_EQ(0, Bind(sock1_, LOCAL_HOST, PORT1));
  EXPECT_EQ(0, Bind(sock2_, LOCAL_HOST, PORT2));

  sockaddr_in addr;
  socklen_t addrlen = sizeof(addr);
  IP4ToSockAddr(LOCAL_HOST, PORT2, &addr);

  int len1 =
     sendto(sock1_, outbuf, sizeof(outbuf), 0, (sockaddr*) &addr, addrlen);
  EXPECT_EQ(sizeof(outbuf), len1);

  // Ensure the buffers are different
  EXPECT_NE(0, memcmp(outbuf, inbuf, sizeof(outbuf)));
  memset(&addr, 0, sizeof(addr));

  // Try to receive the previously sent packet
  int len2 =
    recvfrom(sock2_, inbuf, sizeof(inbuf), 0, (sockaddr*) &addr, &addrlen);
  EXPECT_EQ(sizeof(outbuf), len2);
  EXPECT_EQ(sizeof(sockaddr_in), addrlen);
  EXPECT_EQ(PORT1, htons(addr.sin_port));

  // Now they should be the same
  EXPECT_EQ(0, memcmp(outbuf, inbuf, sizeof(outbuf)));
}

const size_t kQueueSize = 65536 * 8;
TEST_F(SocketTestUDP, FullFifo) {
  char outbuf[16 * 1024];

  ASSERT_EQ(0, Bind(sock1_, LOCAL_HOST, PORT1));
  ASSERT_EQ(0, Bind(sock2_, LOCAL_HOST, PORT2));

  sockaddr_in addr;
  socklen_t addrlen = sizeof(addr);
  IP4ToSockAddr(LOCAL_HOST, PORT2, &addr);

  size_t total = 0;
  while (total < kQueueSize * 8) {
    int len = sendto(sock1_, outbuf, sizeof(outbuf), MSG_DONTWAIT,
                     (sockaddr*) &addr, addrlen);

    if (len <= 0) {
      EXPECT_EQ(-1, len);
      EXPECT_EQ(EWOULDBLOCK, errno);
      break;
    }

    if (len >= 0) {
      EXPECT_EQ(sizeof(outbuf), len);
      total += len;
    }
  }
  EXPECT_GT(total, kQueueSize - 1);
  EXPECT_LT(total, kQueueSize * 8);
}

TEST_F(SocketTestWithServer, TCPConnect) {
  char outbuf[256];
  char inbuf[512];

  memset(outbuf, 1, sizeof(outbuf));

  sockaddr_in addr;
  socklen_t addrlen = sizeof(addr);

  IP4ToSockAddr(LOCAL_HOST, PORT1, &addr);

  ASSERT_EQ(0, connect(sock_, (sockaddr*) &addr, addrlen))
      << "Failed with " << errno << ": " << strerror(errno) << "\n";

  // Send two different messages to the echo server and verify the
  // response matches.
  strcpy(outbuf, "hello");
  memset(inbuf, 0, sizeof(inbuf));
  ASSERT_EQ(sizeof(outbuf), write(sock_, outbuf, sizeof(outbuf)))
      << "socket write failed with: " << strerror(errno);
  ASSERT_EQ(sizeof(outbuf), read(sock_, inbuf, sizeof(inbuf)));
  EXPECT_EQ(0, memcmp(outbuf, inbuf, sizeof(outbuf)));

  strcpy(outbuf, "world");
  memset(inbuf, 0, sizeof(inbuf));
  ASSERT_EQ(sizeof(outbuf), write(sock_, outbuf, sizeof(outbuf)));
  ASSERT_EQ(sizeof(outbuf), read(sock_, inbuf, sizeof(inbuf)));
  EXPECT_EQ(0, memcmp(outbuf, inbuf, sizeof(outbuf)));
}

TEST_F(SocketTestWithServer, TCPConnectNonBlock) {
  char outbuf[256];
  //char inbuf[512];

  memset(outbuf, 1, sizeof(outbuf));

  sockaddr_in addr;
  socklen_t addrlen = sizeof(addr);

  IP4ToSockAddr(LOCAL_HOST, PORT1, &addr);

  SetNonBlocking(sock_);
  ASSERT_EQ(-1, connect(sock_, (sockaddr*) &addr, addrlen));
  ASSERT_EQ(EINPROGRESS, errno)
     << "expected EINPROGRESS but got: " << strerror(errno) << "\n";
  ASSERT_EQ(-1, connect(sock_, (sockaddr*) &addr, addrlen));
  ASSERT_EQ(EALREADY, errno);

  // Wait for the socket connection to complete using poll()
  struct pollfd pollfd = { sock_, POLLIN|POLLOUT, 0 };
  ASSERT_EQ(1, poll(&pollfd, 1, -1));
  ASSERT_EQ(POLLOUT, pollfd.revents);

  // Attempts to connect again should yield EISCONN
  ASSERT_EQ(-1, connect(sock_, (sockaddr*) &addr, addrlen));
  ASSERT_EQ(EISCONN, errno);

  // And SO_ERROR should be 0.
}

TEST_F(SocketTest, Getsockopt) {
  sock1_ = socket(AF_INET, SOCK_STREAM, 0);
  EXPECT_GT(sock1_, -1);
  int socket_error = 99;
  socklen_t len = sizeof(socket_error);

  // Test for valid option (SO_ERROR) which should be 0 when a socket
  // is first created.
  ASSERT_EQ(0, getsockopt(sock1_, SOL_SOCKET, SO_ERROR, &socket_error, &len));
  ASSERT_EQ(0, socket_error);
  ASSERT_EQ(sizeof(socket_error), len);

  // Test for an invalid option (-1)
  ASSERT_EQ(-1, getsockopt(sock1_, SOL_SOCKET, -1, &socket_error, &len));
  ASSERT_EQ(ENOPROTOOPT, errno);
}

TEST_F(SocketTest, Setsockopt) {
  sock1_ = socket(AF_INET, SOCK_STREAM, 0);
  EXPECT_GT(sock1_, -1);

  // It should not be possible to set SO_ERROR using setsockopt.
  int socket_error = 10;
  socklen_t len = sizeof(socket_error);
  ASSERT_EQ(-1, setsockopt(sock1_, SOL_SOCKET, SO_ERROR, &socket_error, len));
  ASSERT_EQ(ENOPROTOOPT, errno);

}

TEST_F(SocketTest, Sockopt_KEEPALIVE) {
  sock1_ = socket(AF_INET, SOCK_STREAM, 0);
  ASSERT_GT(sock1_, -1);
  sock2_ = socket(AF_INET, SOCK_DGRAM, 0);
  ASSERT_GT(sock2_, -1);

  int value = 0;
  socklen_t len = sizeof(value);
  ASSERT_EQ(0, getsockopt(sock1_, SOL_SOCKET, SO_KEEPALIVE, &value, &len));
  ASSERT_EQ(0, value);
  ASSERT_EQ(sizeof(int), len);
}

// Disabled until we support SO_LINGER (i.e. syncronouse close()/shutdown())
// TODO(sbc): re-enable once we fix http://crbug.com/312401
TEST_F(SocketTest, DISABLED_Sockopt_LINGER) {
  sock1_ = socket(AF_INET, SOCK_STREAM, 0);
  ASSERT_GT(sock1_, -1);
  sock2_ = socket(AF_INET, SOCK_DGRAM, 0);
  ASSERT_GT(sock2_, -1);

  struct linger linger = { 7, 8 };
  socklen_t len = sizeof(linger);
  ASSERT_EQ(0, getsockopt(sock1_, SOL_SOCKET, SO_LINGER, &linger, &len));
  ASSERT_EQ(0, linger.l_onoff);
  ASSERT_EQ(0, linger.l_linger);
  ASSERT_EQ(sizeof(struct linger), len);
  ASSERT_EQ(0, getsockopt(sock2_, SOL_SOCKET, SO_LINGER, &linger, &len));
  ASSERT_EQ(0, linger.l_onoff);
  ASSERT_EQ(0, linger.l_linger);
  ASSERT_EQ(sizeof(struct linger), len);

  linger.l_onoff = 1;
  linger.l_linger = 77;
  len = sizeof(linger);
  ASSERT_EQ(0, setsockopt(sock1_, SOL_SOCKET, SO_LINGER, &linger, len));
  linger.l_onoff = 1;
  linger.l_linger = 88;
  ASSERT_EQ(0, setsockopt(sock2_, SOL_SOCKET, SO_LINGER, &linger, len));

  len = sizeof(linger);
  ASSERT_EQ(0, getsockopt(sock1_, SOL_SOCKET, SO_LINGER, &linger, &len));
  ASSERT_EQ(1, linger.l_onoff);
  ASSERT_EQ(77, linger.l_linger);
  ASSERT_EQ(sizeof(struct linger), len);
  ASSERT_EQ(0, getsockopt(sock2_, SOL_SOCKET, SO_LINGER, &linger, &len));
  ASSERT_EQ(1, linger.l_onoff);
  ASSERT_EQ(88, linger.l_linger);
  ASSERT_EQ(sizeof(struct linger), len);
}

TEST_F(SocketTest, Sockopt_REUSEADDR) {
  int value = 1;
  socklen_t len = sizeof(value);
  sock1_ = socket(AF_INET, SOCK_STREAM, 0);

  ASSERT_GT(sock1_, -1);
  ASSERT_EQ(0, setsockopt(sock1_, SOL_SOCKET, SO_REUSEADDR, &value, len));

  value = 0;
  len = sizeof(value);
  ASSERT_EQ(0, getsockopt(sock1_, SOL_SOCKET, SO_REUSEADDR, &value, &len));
  ASSERT_EQ(1, value);
  ASSERT_EQ(sizeof(int), len);
}

// The size of the data to send is deliberately chosen to be
// larger than the TCP buffer in nacl_io.
// TODO(sbc): use ioctl to discover the actual buffer size at
// runtime.
#define LARGE_SEND_BYTES (800 * 1024)
TEST_F(SocketTestWithServer, LargeSend) {
  char* outbuf = (char*)malloc(LARGE_SEND_BYTES);
  char* inbuf = (char*)malloc(LARGE_SEND_BYTES);
  int bytes_sent = 0;
  int bytes_received = 0;

  // Fill output buffer with ascending integers
  int* outbuf_int = (int*)outbuf;
  int* inbuf_int = (int*)inbuf;
  for (int i = 0; i < LARGE_SEND_BYTES/sizeof(int); i++) {
    outbuf_int[i] = i;
  }

  sockaddr_in addr;
  socklen_t addrlen = sizeof(addr);

  IP4ToSockAddr(LOCAL_HOST, PORT1, &addr);
  ASSERT_EQ(0, connect(sock_, (sockaddr*) &addr, addrlen))
      << "Failed with " << errno << ": " << strerror(errno) << "\n";

  // Call send an recv until all bytes have been transfered.
  while (bytes_received < LARGE_SEND_BYTES) {
    if (bytes_sent < LARGE_SEND_BYTES) {
      int sent = send(sock_, outbuf + bytes_sent,
                      LARGE_SEND_BYTES - bytes_sent, MSG_DONTWAIT);
      if (sent < 0)
        ASSERT_EQ(EWOULDBLOCK, errno) << "send failed: " << strerror(errno);
      else
        bytes_sent += sent;
    }

    int received = recv(sock_, inbuf + bytes_received,
                        LARGE_SEND_BYTES - bytes_received, MSG_DONTWAIT);
    if (received < 0)
      ASSERT_EQ(EWOULDBLOCK, errno) << "recv failed: " << strerror(errno);
    else
      bytes_received += received;
  }

  // Make sure there is nothing else to recv at this point
  char dummy[10];
  ASSERT_EQ(-1, recv(sock_, dummy, 10, MSG_DONTWAIT));
  ASSERT_EQ(EWOULDBLOCK, errno);

  int errors = 0;
  for (int i = 0; i < LARGE_SEND_BYTES/4; i++) {
    if (inbuf_int[i] != outbuf_int[i]) {
      printf("%d: in=%d out=%d\n", i, inbuf_int[i], outbuf_int[i]);
      if (errors++ > 50)
        break;
    }
  }

  for (int i = 0; i < LARGE_SEND_BYTES; i++) {
    ASSERT_EQ(outbuf[i], inbuf[i]) << "cmp failed at " << i;
  }

  ASSERT_EQ(0, memcmp(inbuf, outbuf, LARGE_SEND_BYTES));

  free(inbuf);
  free(outbuf);
}

TEST_F(SocketTestUDP, Listen) {
  EXPECT_EQ(-1, listen(sock1_, 10));
  EXPECT_EQ(errno, ENOTSUP);
}

TEST_F(SocketTestTCP, Listen) {
  sockaddr_in addr;
  socklen_t addrlen = sizeof(addr);
  const char* client_greeting = "hello";
  const char* server_reply = "reply";
  const int greeting_len = strlen(client_greeting);
  const int reply_len = strlen(server_reply);

  int server_sock = sock1_;

  // Accept before listen should fail
  ASSERT_EQ(-1, accept(server_sock, (sockaddr*)&addr, &addrlen));

  // Listen should fail on unbound socket
  ASSERT_EQ(-1, listen(server_sock, 10));

  // Bind and Listen
  ASSERT_EQ(0, Bind(server_sock, LOCAL_HOST, PORT1));
  ASSERT_EQ(0, listen(server_sock, 10))
    << "listen failed with: " << strerror(errno);

  // Connect to listening socket, and send greeting
  int client_sock = sock2_;
  IP4ToSockAddr(LOCAL_HOST, PORT1, &addr);
  addrlen = sizeof(addr);
  ASSERT_EQ(0, connect(client_sock, (sockaddr*)&addr, addrlen))
    << "Failed with " << errno << ": " << strerror(errno) << "\n";

  ASSERT_EQ(greeting_len, send(client_sock, client_greeting,
                               greeting_len, 0));

  // Pass in addrlen that is larger than our actual address to make
  // sure that it is correctly set back to sizeof(sockaddr_in)
  addrlen = sizeof(addr) + 10;
  int new_socket = accept(server_sock, (sockaddr*)&addr, &addrlen);
  ASSERT_GT(new_socket, -1)
    << "accept failed with " << errno << ": " << strerror(errno) << "\n";

  // Verify addr and addrlen were set correctly
  ASSERT_EQ(addrlen, sizeof(sockaddr_in));
  sockaddr_in client_addr;
  ASSERT_EQ(0, getsockname(client_sock, (sockaddr*)&client_addr, &addrlen));
  ASSERT_EQ(client_addr.sin_family, addr.sin_family);
  ASSERT_EQ(client_addr.sin_port, addr.sin_port);
  ASSERT_EQ(client_addr.sin_addr.s_addr, addr.sin_addr.s_addr);

  // Recv greeting from client and send reply
  char inbuf[512];
  ASSERT_EQ(greeting_len, recv(new_socket, inbuf, sizeof(inbuf), 0));
  ASSERT_STREQ(inbuf, client_greeting);
  ASSERT_EQ(reply_len, send(new_socket, server_reply, reply_len, 0));

  // Recv reply on client socket
  ASSERT_EQ(reply_len, recv(client_sock, inbuf, sizeof(inbuf), 0));
  ASSERT_STREQ(inbuf, server_reply);

  ASSERT_EQ(0, close(new_socket));
}

TEST_F(SocketTestTCP, ListenNonBlocking) {
  int server_sock = sock1_;

  // Set non-blocking
  SetNonBlocking(server_sock);

  // bind and listen
  ASSERT_EQ(0, Bind(server_sock, LOCAL_HOST, PORT1));
  ASSERT_EQ(0, listen(server_sock, 10))
    << "listen failed with: " << strerror(errno);

  // Accept should fail with EAGAIN since there is no incomming
  // connection.
  sockaddr_in addr;
  socklen_t addrlen = sizeof(addr);
  ASSERT_EQ(-1, accept(server_sock, (sockaddr*)&addr, &addrlen));
  ASSERT_EQ(EAGAIN, errno);

  // If we poll the listening socket it should also return
  // not readable to indicate that no connections are available
  // to accept.
  struct pollfd pollfd = { server_sock, POLLIN|POLLOUT, 0 };
  ASSERT_EQ(0, poll(&pollfd, 1, 0));

  // Connect to listening socket
  int client_sock = sock2_;
  IP4ToSockAddr(LOCAL_HOST, PORT1, &addr);
  addrlen = sizeof(addr);
  ASSERT_EQ(0, connect(client_sock, (sockaddr*)&addr, addrlen))
    << "Failed with " << errno << ": " << strerror(errno) << "\n";

  // Not poll again but with an infintie timeout.
  pollfd.fd = server_sock;
  pollfd.events = POLLIN | POLLOUT;
  ASSERT_EQ(1, poll(&pollfd, 1, -1));

  // Now non-blocking accept should return the new socket
  int new_socket = accept(server_sock, (sockaddr*)&addr, &addrlen);
  ASSERT_NE(-1, new_socket)
    << "accept failed with: " << strerror(errno) << "\n";
  ASSERT_EQ(0, close(new_socket));

  // Accept calls should once again fail with EAGAIN
  ASSERT_EQ(-1, accept(server_sock, (sockaddr*)&addr, &addrlen));
  ASSERT_EQ(EAGAIN, errno);

  // As should polling the listening socket
  pollfd.fd = server_sock;
  pollfd.events = POLLIN | POLLOUT;
  ASSERT_EQ(0, poll(&pollfd, 1, 0));
}

TEST_F(SocketTestTCP, SendRecvAfterRemoteShutdown) {
  sockaddr_in addr;
  socklen_t addrlen = sizeof(addr);

  int server_sock = sock1_;
  int client_sock = sock2_;

  // bind and listen
  ASSERT_EQ(0, Bind(server_sock, LOCAL_HOST, PORT1));
  ASSERT_EQ(0, listen(server_sock, 10))
    << "listen failed with: " << strerror(errno);

  // connect to listening socket
  IP4ToSockAddr(LOCAL_HOST, PORT1, &addr);
  ASSERT_EQ(0, connect(client_sock, (sockaddr*)&addr, addrlen))
    << "Failed with " << errno << ": " << strerror(errno) << "\n";

  addrlen = sizeof(addr);
  int new_sock = accept(server_sock, (sockaddr*)&addr, &addrlen);
  ASSERT_NE(-1, new_sock);

  const char* send_buf = "hello world";
  ASSERT_EQ(strlen(send_buf), send(new_sock, send_buf, strlen(send_buf), 0));

  // Recv first 10 bytes
  char buf[256];
  ASSERT_EQ(10, recv(client_sock, buf, 10, 0));

  // Close the new socket
  ASSERT_EQ(0, close(new_sock));

  // Recv remainder
  int bytes_remaining = strlen(send_buf) - 10;
  ASSERT_EQ(bytes_remaining, recv(client_sock, buf, 256, 0));

  // Attempt to read/write after remote shutdown, with no bytes remainging
  ASSERT_EQ(0, recv(client_sock, buf, 10, 0));
  ASSERT_EQ(0, recv(client_sock, buf, 10, 0));
  ASSERT_EQ(-1, send(client_sock, buf, 10, 0));
  ASSERT_EQ(errno, EPIPE);
}

TEST_F(SocketTestTCP, SendRecvAfterLocalShutdown) {
  sockaddr_in addr;
  socklen_t addrlen = sizeof(addr);

  int server_sock = sock1_;
  int client_sock = sock2_;

  // bind and listen
  ASSERT_EQ(0, Bind(server_sock, LOCAL_HOST, PORT1));
  ASSERT_EQ(0, listen(server_sock, 10))
    << "listen failed with: " << strerror(errno);

  // connect to listening socket
  IP4ToSockAddr(LOCAL_HOST, PORT1, &addr);
  ASSERT_EQ(0, connect(client_sock, (sockaddr*)&addr, addrlen))
    << "Failed with " << errno << ": " << strerror(errno) << "\n";

  addrlen = sizeof(addr);
  int new_sock = accept(server_sock, (sockaddr*)&addr, &addrlen);
  ASSERT_NE(-1, new_sock);

  // Close the new socket
  ASSERT_EQ(0, shutdown(client_sock, SHUT_RDWR));

  // Attempt to read/write after shutdown
  char buffer[10];
  ASSERT_EQ(0, recv(client_sock, buffer, sizeof(buffer), 0));
  ASSERT_EQ(-1, send(client_sock, buffer, sizeof(buffer), 0));
  ASSERT_EQ(errno, EPIPE);
}

#define SEND_BYTES (1024)
TEST_F(SocketTestTCP, SendBufferedDataAfterShutdown) {
  sockaddr_in addr;
  socklen_t addrlen = sizeof(addr);

  int server_sock = sock1_;
  int client_sock = sock2_;

  // bind and listen
  ASSERT_EQ(0, Bind(server_sock, LOCAL_HOST, PORT1));
  ASSERT_EQ(0, listen(server_sock, 10))
    << "listen failed with: " << strerror(errno);

  // connect to listening socket
  IP4ToSockAddr(LOCAL_HOST, PORT1, &addr);
  ASSERT_EQ(0, connect(client_sock, (sockaddr*)&addr, addrlen))
    << "Failed with " << errno << ": " << strerror(errno) << "\n";

  addrlen = sizeof(addr);
  int new_sock = accept(server_sock, (sockaddr*)&addr, &addrlen);
  ASSERT_NE(-1, new_sock);

  // send a fairly large amount of data and immediately close
  // the socket.
  void* buffer = alloca(SEND_BYTES);
  ASSERT_EQ(SEND_BYTES, send(client_sock, buffer, SEND_BYTES, 0));
  ASSERT_EQ(0, close(client_sock));

  // avoid double close of sock2_
  sock2_ = -1;

  // Attempt to recv() all the sent data.  None should be lost.
  int remainder = SEND_BYTES;
  while (remainder > 0) {
    int rtn = recv(new_sock, buffer, remainder, 0);
    ASSERT_GT(rtn, 0);
    remainder -= rtn;
  }

  ASSERT_EQ(0, close(new_sock));
}

#endif  // PROVIDES_SOCKET_API
