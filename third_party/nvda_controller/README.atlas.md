# NVDA controller interface

Atlas speaks its announcements on Windows through NVDA's controller interface,
the same RPC interface NVDA's own `nvdaControllerClient.dll` calls. Atlas builds
the client side itself instead of shipping that DLL.

- Source: NVDA by NV Access, https://github.com/nvaccess/nvda at 732d1ce
  (October 2026).
- `nvdaController.idl` and `nvdaController.acf` are copied unchanged from
  `nvdaHelper/interfaces/nvdaController/`. They are licensed under the GNU
  Lesser General Public License version 2.1 (`LICENSE`, copied from
  `extras/controllerClient/license.txt`), which allows their use in Atlas
  under the GPL v3.
- At build time MIDL generates the RPC client stub from them
  (`nvdaController.h` and `nvdaController_c.c` in the build folder), with the
  flags NVDA's own build uses for its client: `/server none` and
  `/prefix all nvdaController_`.
- `src/plugin/windows_screen_reader.cpp` replaces NVDA's `client.cpp`: it binds
  to the same `ncalrpc:[NvdaCtlr.<session>.<desktop>]` endpoint that NVDA's
  `generateDesktopSpecificNamespace` names, and provides the allocator and the
  SSML mark callback the stub needs.

Atlas calls `speakText` and `brailleMessage` from the first interface, which
every NVDA since 2011 serves. It does not use the SSML or `isSpeaking` calls of
the later interfaces.
