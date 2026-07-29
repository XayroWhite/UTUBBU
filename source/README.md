# UTUBBU source code

This directory contains the complete public project source:

- `psp/`: PSP application written in C and MIPS assembly;
- `assets/`: XMB artwork and Roboto font used by the build;
- `preview/`: browser preview source;
- `tools/`: development and media-building utilities;
- `vendor.rar`: third-party PSP libraries, headers and corresponding notices.

No maintainer credentials, cookies, API keys, private tokens, local absolute
paths, email addresses, or other personal data are included. Third-party files
retain only their original copyright and attribution notices.

## Build

Install PSPSDK, curl and mbedTLS. Extract `vendor.rar` in this directory, then:

```sh
cd psp
make
```

The resulting executable is `psp/EBOOT.PBP`. See the repository root
`THIRD_PARTY.md` for dependency licenses and source-availability requirements.
