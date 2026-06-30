#ifndef TLS13_TEST_H_
#define TLS13_TEST_H_

/** HTTPS GET to howsmyssl.com with TLS 1.3 only; logs result. Returns true on success. */
bool RunTls13Test();

/** HTTPS GET via Board network Http (CreateHttp); logs result. Returns true on success. */
bool RunHttpTest();

#endif  // TLS13_TEST_H_
