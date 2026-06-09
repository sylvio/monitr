{
  "targets": [
    {
      "target_name": "monitor",
      "sources": [ "src/monitor.cc" ],
      "include_dirs": [
          "<!@(node -p \"require('node-addon-api').include\")"
      ],
      "defines": [
          "NAPI_VERSION=8",
          "NAPI_CPP_EXCEPTIONS"
      ],
      "cflags_cc": [ "-fexceptions" ],
      "cflags_cc!": [ "-fno-exceptions" ]
    }
  ]
}
