#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#ifdef HAVE_MEMORY_H
#include <memory.h>
#endif
#include <errno.h>
#ifdef HAVE_STRING_H
#include <string.h>
#endif
#include <getopt.h>
#include <stdint.h>
#include <poll.h>
#include <time.h>
#include <sys/types.h>
#include <sys/wait.h>
#ifdef HAVE_SYS_SOCKET_H
#include <sys/socket.h>
#endif
#include <sys/poll.h>
#include <sys/un.h>
#include <sys/stat.h>
#ifdef HAVE_FCNTL_H
#include <fcntl.h>
#endif
#include <netinet/in.h>
#include <arpa/inet.h>
#ifdef HAVE_NETDB_H
#include <netdb.h>
#endif
#include <getopt.h>
#ifdef HAVE_ALLOCA_H
#include <alloca.h>
#endif
#include <set>
#include <map>
#include <iostream>

//#define _DEBUG 1

unsigned max_delay = 1000;

bool global_shutdown_flag = false;
bool daemonize = true;

const unsigned min_delay_ns = 10000;

using namespace std;

class IpAddr
{
public:
  unsigned val;
  IpAddr() : val(0) {}
  IpAddr(const IpAddr &x) : val(x.val) {}
  IpAddr(const struct sockaddr_in &a) : val(*((unsigned *)&a.sin_addr)) {}
  IpAddr(const char *s) { val = inet_addr(s); }
  void setAddrTo(struct sockaddr_in *a) const { *((unsigned *)&(a->sin_addr)) = val; }
  bool operator==(const IpAddr &x) const { return x.val == val; }
  bool operator<(const IpAddr &x) const { return val < x.val; }
  bool operator>(const IpAddr &x) const { return val > x.val; }
};
std::ostream &operator<<(std::ostream &os, const IpAddr &x)
{
  os << (x.val & 0xff) << "." << ((x.val >> 8) & 0xff) << "." << ((x.val >> 16) & 0xff) << "." << (x.val >> 24);
  return os;
}

class IpAddrPort
{
public:
  IpAddr addr;
  unsigned short port;
  IpAddrPort() : port(0) {}
  IpAddrPort(const IpAddr &a, unsigned short _port) : addr(a), port(_port) {}
  IpAddrPort(const struct sockaddr_in &a) : addr(a) { port = ntohs(a.sin_port); }
  IpAddrPort(const char *s);
  void setAddrPortTo(struct sockaddr_in *a) const
  {
    addr.setAddrTo(a);
    a->sin_port = htons(port);
  }
  bool operator==(const IpAddrPort &x) const { return addr == x.addr && port == x.port; }
  bool operator<(const IpAddrPort &x) const
  {
    if (addr < x.addr)
      return true;
    if (addr > x.addr)
      return false;
    return port < x.port;
  }
  bool operator>(const IpAddrPort &x) const
  {
    if (addr > x.addr)
      return true;
    if (addr < x.addr)
      return false;
    return port > x.port;
  }
};
IpAddrPort::IpAddrPort(const char *s) : port(0)
{
  if (!s)
    return;
  const char *q;
  for (q = s; *q && *q != ':'; q++)
    ;
  addr = IpAddr(s);
  if (*q == ':')
  {
    port = strtol(q + 1, 0, 10);
  }
}

std::ostream &operator<<(std::ostream &os, const IpAddrPort &x)
{
  os << x.addr << ":" << x.port;
  return os;
}

class UdpSocket
{
public:
  mutable int fd;
  UdpSocket();
  UdpSocket(int q);
  UdpSocket(const UdpSocket &);
  UdpSocket &operator=(const UdpSocket &);
  ~UdpSocket();
  void setfd(int q)
  {
    if (fd >= 0)
      close(fd);
    fd = q;
  }
};

UdpSocket::UdpSocket() : fd(-1)
{
}
UdpSocket::UdpSocket(int q) : fd(q)
{
}
UdpSocket::UdpSocket(const UdpSocket &x)
{
  fd = x.fd;
  x.fd = -1;
}
UdpSocket &UdpSocket::operator=(const UdpSocket &x)
{
  fd = x.fd;
  x.fd = -1;
  return *this;
}

UdpSocket::~UdpSocket()
{
  if (fd >= 0)
    close(fd);
  fd = -1;
}

class BfdSession
{
public:
  mutable unsigned state;
  mutable struct timespec lastsent, lastrecv;
  IpAddrPort peer;
  unsigned short target_port;
  unsigned dsc_my;
  unsigned dsc_their, mult, tx, rx;
  BfdSession(const struct timespec &, struct sockaddr_in *from, unsigned short tport, const char *szPack);
  bool operator==(const BfdSession &x) const;
  bool operator<(const BfdSession &x) const;
  const char *tostr(char *) const;
  int need_to_send_in_msec(const struct timespec &) const;
  ssize_t send(const struct timespec &, int) const;
  void recv(const struct timespec &, const char *, ssize_t) const;
  void shutdown() const;
  bool is_down() const;
  bool is_stale(const struct timespec &) const;
};

int sub_msec(const struct timespec &from, const struct timespec &sub)
{
  return (from.tv_sec - sub.tv_sec) * 1000 + (from.tv_nsec - sub.tv_nsec) / 1000000;
}
void BfdSession::shutdown() const
{
  if (state == 1)
  {
    state = 2;
    return;
  }
  if (state == 0)
  {
    state = 3;
    return;
  }
}
bool BfdSession::is_down() const { return state == 3; }

BfdSession::BfdSession(const struct timespec &now, struct sockaddr_in *from, unsigned short tport, const char *szPack)
{
  state = 0;
  lastsent = now;
  lastrecv = now;
  peer = IpAddrPort(*from);
  target_port = tport;
  mult = szPack[2];
  dsc_their = ntohl(*((uint32_t *)(szPack + 4)));
  dsc_my = ntohl(*((uint32_t *)(szPack + 8)));
  rx = ntohl(*((uint32_t *)(szPack + 12)));
  tx = ntohl(*((uint32_t *)(szPack + 16)));
  if (!dsc_my)
    dsc_my = dsc_their + 1;
  if (tx < min_delay_ns)
    tx = min_delay_ns;
  if (rx < min_delay_ns)
    rx = min_delay_ns;
  unsigned md = max_delay * 1000;
  if (tx > md)
    tx = md;
  if (rx > md)
    rx = md;
}
bool BfdSession::operator==(const BfdSession &x) const
{
  return peer == x.peer && dsc_their == x.dsc_their;
}
bool BfdSession::operator<(const BfdSession &x) const
{
  if (peer < x.peer)
    return true;
  if (peer > x.peer)
    return false;
  if (dsc_their < x.dsc_their)
    return true;
  return false;
}
std::ostream &operator<<(std::ostream &os, const BfdSession &x)
{
  os << "BFD[" << x.peer << " -: " << x.target_port << " " << x.dsc_their << "-" << x.dsc_my << " mult=" << x.mult << " tx=" << x.tx << " rx=" << x.rx << "]";
  return os;
}
int BfdSession::need_to_send_in_msec(const struct timespec &now) const
{
  if (state == 0)
    return 0;
  if (state == 1)
  {
    int df = sub_msec(now, lastsent);
    if ((df > (tx / 1000)) || df < 0)
      return 0;
    return (tx / 1000) - df;
  }
  if (state == 2)
    return 0;
  return -1;
}
bool BfdSession::is_stale(const struct timespec &now) const
{
  int df = sub_msec(now, lastrecv);
  return df > (mult * (rx / 1000));
}

ssize_t BfdSession::send(const struct timespec &now, int iSocket) const
{
  char szBuf[256];
  char buf[24];
  struct sockaddr_in sck;
  sck.sin_family = AF_INET;
  buf[0] = 0x20;
  buf[2] = mult;
  buf[3] = 24;
  *((uint32_t *)(buf + 4)) = htonl(dsc_my);
  *((uint32_t *)(buf + 8)) = htonl(dsc_their);
  *((uint32_t *)(buf + 12)) = htonl(tx);
  *((uint32_t *)(buf + 16)) = htonl(rx);
  *((uint32_t *)(buf + 20)) = 0;
  switch (state)
  {
  case 0: //init
    buf[1] = 0x80;
    state++;
    break;
  case 1: //up
    buf[1] = 0xc0;
    break;
  case 2: //down
    buf[1] = 0x40;
    state++;
    break;
  }
  peer.addr.setAddrTo(&sck);
  sck.sin_port = htons(target_port);
  lastsent = now;
#ifdef _DEBUG
  cout << "Sending " << *this << " to " << peer.addr << ":" << target_port << endl;
#endif
  return sendto(iSocket, buf, 24, 0, (const struct sockaddr *)&sck, sizeof(sck));
}
void BfdSession::recv(const struct timespec &now, const char *buf, ssize_t sz) const
{
#ifdef _DEBUG
  cout << "Received " << sz << " for " << *this << endl;
#endif
  lastrecv = now;
}

class BfdDispatcher
{
public:
  bool shutting_down;
  std::map<IpAddrPort, UdpSocket> sockets;
  std::set<BfdSession> sessions;
  BfdDispatcher() : shutting_down(false) {}
  int add_listen(const IpAddrPort &);
  void lifecycle();
};

int BfdDispatcher::add_listen(const IpAddrPort &addrport)
{
  int iSocket = socket(PF_INET, SOCK_DGRAM, 17);
  if (iSocket == -1)
    return errno;
  /*
  if(setsockopt(iSocket,SOL_SOCKET,SO_REUSEADDR,&flags,sizeof(flags))){
        perror("Unable to setsockopt");
  }
  */
  int err;
  struct sockaddr_in bindaddr;
  memset(&bindaddr, 0, sizeof(bindaddr));
  bindaddr.sin_family = AF_INET;
  addrport.setAddrPortTo(&bindaddr);
  if (bind(iSocket, (struct sockaddr *)&bindaddr, sizeof(struct sockaddr_in)) < 0)
  {
    err = errno;
    perror("Unable to bind");
    close(iSocket);
    return err;
  }
  int flags = fcntl(iSocket, F_GETFL, 0);
  if (flags == -1)
    flags = 0;
  fcntl(iSocket, F_SETFL, flags | O_NONBLOCK);
  sockets[addrport] = UdpSocket(iSocket);
  return 0;
}

void BfdDispatcher::lifecycle()
{
  char szBuf[256], szStr[256];
  if (sockets.size() < 1)
    return;
  int flags, i;
  ssize_t szRcv;
  std::set<BfdSession> sessions;
  struct sockaddr_in rcvd_addr;
  socklen_t rcvd_len, resp_len;
  struct pollfd *fds = (struct pollfd *)alloca(sizeof(struct pollfd) * sockets.size());
  struct timespec now;
  i = 0;
  for (std::map<IpAddrPort, UdpSocket>::iterator scki = sockets.begin(); i < sockets.size() && scki != sockets.end(); i++, scki++)
  {
#ifdef _DEBUG
    cout << "Listening at " << scki->first << " - " << scki->second.fd << endl;
#endif
    fds[i].fd = scki->second.fd;
  }
  for (;;)
  {
    std::set<BfdSession>::iterator si, siout;
    flags = POLLIN | POLLHUP;
    ;
    int wait = -1;
    if (clock_gettime(CLOCK_REALTIME, &now))
    {
      perror("clock_gettime");
    }
    for (;;)
    {
      bool _rest = false;
      for (si = sessions.begin(); si != sessions.end(); si++)
      {
        if (si->is_stale(now))
        {
          sessions.erase(si);
          _rest = true;
          break;
        }
        if (shutting_down)
          si->shutdown();
        int w = si->need_to_send_in_msec(now);
        if (w >= 0 && (w < wait || wait == -1))
          wait = w;
      }
      if (!_rest)
        break;
    }
    /*    
#ifdef _DEBUG
    cout << "Next event in " << wait << " msec" << endl;
#endif
*/
    if (wait >= 0 && wait < 1000)
      flags |= POLLOUT;
    else
      wait = 1000;
    for (int i = 0; i < sockets.size(); i++)
    {
      fds[i].events = flags;
      fds[i].revents = 0;
    }
    /*
#ifdef _DEBUG
    cout << "Wait for " << wait << " msec" << endl;
#endif
*/
    int rt = poll(fds, sockets.size(), wait);
    if (rt < 0)
      break; //error
    if (rt == 0)
      continue; //timeout
    if (global_shutdown_flag)
      shutting_down = true;
    if (clock_gettime(CLOCK_REALTIME, &now))
    {
      perror("clock_gettime");
    }
    int fdn = 0;
    for (std::map<IpAddrPort, UdpSocket>::iterator scki = sockets.begin(); fdn < sockets.size() && scki != sockets.end(); fdn++, scki++)
    {
      if (fds[fdn].revents & POLLIN)
      {
        szRcv = recvfrom(fds[fdn].fd, szBuf, sizeof(szBuf), 0 /*MSG_WAITALL | MSG_TRUNC*/, (sockaddr *)&rcvd_addr, &rcvd_len);
        if (szRcv < 0)
        {
          if (errno != EAGAIN)
            break;
          szRcv = 0;
        }
#ifdef _DEBUG
        if (szRcv > 0)
          cout << "Received " << szRcv << " bytes from " << IpAddrPort(rcvd_addr) << endl;
#endif
        BfdSession sess(now, &rcvd_addr, scki->first.port, szBuf);
        si = sessions.find(sess);
        if (si == sessions.end())
        {
          if (!shutting_down)
          {
#ifdef _DEBUG
            cout << "New session: " << sess << endl;
#endif
            sessions.insert(sess);
          }
          si = sessions.find(sess);
        }
        else
        {
          si->recv(now, szBuf, szRcv);
        }
      }
      if (fds[fdn].revents & POLLOUT)
      {
        siout = sessions.begin();
        int waitsel = siout->need_to_send_in_msec(now);
        for (si = sessions.begin(); si != sessions.end(); si++)
        {
          int waitcur = si->need_to_send_in_msec(now);
          if (waitcur >= 0 && (waitcur < waitsel || waitsel < 0))
          {
            waitsel = waitcur;
            siout = si;
          }
        }
        if (waitsel >= 0)
        {
          if (siout->send(now, fds[fdn].fd) < 1)
          {
            perror("Send response error");
          }
        }
      }
    }
    for (;;)
    {
      bool _rest = false;
      for (si = sessions.begin(); si != sessions.end(); si++)
      {
        if (si->is_down())
        {
          sessions.erase(si);
          _rest = true;
          break;
        }
      }
      if (!_rest)
        break;
    }
    if (sessions.size() < 1 && shutting_down)
      break;
  }
};

void shutdown_global(int n)
{
  cerr << "signal " << n << " received" << endl;
  global_shutdown_flag = true;
}

int main(int argc, char **argv)
{
  BfdDispatcher dsp;
  int i, j;
  const char *szPidFile = 0;
  while ((i = getopt(argc, argv, "hfp:l:")) > 0)
  {
    switch (i)
    {
    case 'h':
    {
      cerr << argv[0] << endl
           << " -h - this help" << endl
           << " -f - does not daemonize" << endl
           << " -d 1000 max allowed delay for peers, ms" << endl
           << " -p pid file " << endl
           << " -l 0.0.0.0:3784 - listen at" << endl;
      return 0;
    }
    case 'f':
      daemonize = false;
      break;
    case 'p':
      szPidFile = optarg;
      break;
    case 'd':
      sscanf(optarg, "%u", &max_delay);
      break;
    case 'l':
    {
      IpAddrPort adrp(optarg);
      if (adrp.port != 0)
        dsp.add_listen(adrp);
      else
      {
        adrp.port = 3784;
        dsp.add_listen(adrp);
        adrp.port = 4784;
        dsp.add_listen(adrp);
      }
      break;
    }
    }
  }
  if (daemonize)
  {
    i = fork();
    if (i < 0)
    {
      perror("Fork failed");
      return i;
    }
    if (i > 0)
    {
      waitpid(i, &j, WNOHANG);
      return 0;
    }
    setsid();
  }
  if (dsp.sockets.size() < 1)
  {
    dsp.add_listen(IpAddrPort(IpAddr("0.0.0.0"), 3784));
    dsp.add_listen(IpAddrPort(IpAddr("0.0.0.0"), 4784));
  }
  signal(SIGTERM, shutdown_global);
  signal(SIGINT, shutdown_global);
  if (szPidFile)
  {
    FILE *fPid = fopen(szPidFile, "w");
    if (fPid)
    {
      fprintf(fPid, "%u\n", getpid());
      fclose(fPid);
    }
    else
      perror("open pid file");
  }
  if (daemonize)
  {
    i = open("/dev/null", O_RDWR);
    chdir("/");
    close(2);
    close(1);
    close(0);
    dup2(i, 0);
    dup2(i, 1);
    dup2(i, 2);
    close(i);
  }
  dsp.lifecycle();
  if (szPidFile)
    unlink(szPidFile);
  return 0;
}