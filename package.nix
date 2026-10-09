{
  lib,
  stdenv,
  cmake,
  qt6,
  chrony,
}:
stdenv.mkDerivation {
  pname = "watchrate";
  version = "0.1.0";
  src = lib.cleanSource ./.;
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
    mkdir -m 700 "$XDG_RUNTIME_DIR" "$XDG_CONFIG_HOME"
    ctest --output-on-failure
    runHook postCheck
  '';
  meta = {
    description = "Native single-click watch offset and rate measurements";
    mainProgram = "watchrate";
    platforms = lib.platforms.linux;
  };
}
