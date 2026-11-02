// Singularity's additions to Mbed TLS's default configuration, for the
// Office connection (code/backend/Office/OfficeConnection): a TLS server on
// this computer's loopback that Word, Excel and Outlook load Singularity's
// panes from, and the certificate it presents (Office/OfficeCertificate).
//
// TLS 1.3 is left out: Mbed TLS 3.6 carries it through PSA Crypto, whose
// global state is not safe across threads without MBEDTLS_THREADING_C, and
// each of the server's connections runs on its own thread. Every connection
// keeps its own configuration, random generator and parsed key, so nothing
// is shared between threads. TLS 1.2 with ECDHE and AES-GCM is what the
// panes' WebView2 and WebKit negotiate with it.
#undef MBEDTLS_SSL_PROTO_TLS1_3
#undef MBEDTLS_SSL_TLS1_3_COMPATIBILITY_MODE
#undef MBEDTLS_SSL_EARLY_DATA
