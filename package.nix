{
  lib,
  stdenv,
  cmake,
  qt6,
  chrony,
  makeFontsConf,
}:
stdenv.mkDerivation {
  pname = "watchrate";
  version = "0.3.0";
  src = lib.cleanSource ./.;
  env.LANG = "C.UTF-8";
  nativeBuildInputs = [
    cmake
    qt6.wrapQtAppsHook
  ];
  buildInputs = [
    qt6.qtbase
    qt6.qtwayland
  ];
  qtWrapperArgs = [
    "--prefix"
    "PATH"
    ":"
    (lib.makeBinPath [ chrony ])
  ];
  doCheck = true;
  checkPhase = ''
    runHook preCheck
    export QT_QPA_PLATFORM=offscreen
    export QT_QPA_PLATFORMTHEME=""
    export XDG_RUNTIME_DIR="$TMPDIR/runtime"
    export XDG_CONFIG_HOME="$TMPDIR/config"
    export XDG_CACHE_HOME="$TMPDIR/cache"
    export FONTCONFIG_FILE="${
      makeFontsConf {
        fontDirectories = [ ];
        impureFontDirectories = [ ];
        includes = [ ];
      }
    }"
    mkdir -m 700 "$XDG_RUNTIME_DIR" "$XDG_CONFIG_HOME" "$XDG_CACHE_HOME"
    ctest --verbose
    runHook postCheck
  '';
  meta = {
    description = "Native single-click watch offset and rate measurements";
    mainProgram = "watchrate";
    platforms = lib.platforms.linux;
    license = lib.licenses.mit;
  };
}
