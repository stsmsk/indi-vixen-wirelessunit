# Copyright (C) 2026 Satoshi Masuko <wakeup1002@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later
# Licensed under the GNU General Public License, version 3 or later.
# See COPYING for the full license text.

"""Prevent package-version and current manual-rate documentation drift."""

from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[1]


def test_retired_environment_specific_fixtures_do_not_return():
    """Keep previously published environment-specific values out of the source tree."""
    forbidden_values = (
        "139." + "997806",
        "37." + "053968",
        "aterm-" + "e5dc0e-g",
        "p" + "pap",
        "VixenWirelessUnit" + "2597",
    )
    text_suffixes = {
        ".cpp", ".hpp", ".py", ".sh", ".cmake", ".in", ".md", ".txt", ".1",
    }
    for path in ROOT.rglob("*"):
        if not path.is_file() or path.suffix not in text_suffixes:
            continue
        relative = path.relative_to(ROOT)
        if any(part == ".git" or part.startswith("build") for part in relative.parts):
            continue
        try:
            content = path.read_text(encoding="utf-8")
        except UnicodeDecodeError as error:
            raise AssertionError(f"Audited source file is not UTF-8: {relative}") from error
        for value in forbidden_values:
            assert value not in content, f"Retired environment-specific value found in {relative}"


def test_gpl_notices_and_license_text():
    copying = (ROOT / "COPYING").read_text(encoding="utf-8")
    assert copying.startswith("GNU GENERAL PUBLIC LICENSE\nVersion 3")
    assert "END OF TERMS AND CONDITIONS" in copying

    code_files = [ROOT / "CMakeLists.txt"]
    for directory in ("src", "tests", "scripts", "tools", "cmake"):
        code_files.extend(
            path for path in (ROOT / directory).iterdir()
            if path.is_file() and not path.name.startswith(".") and
            path.suffix in {".cpp", ".hpp", ".py", ".sh", ".cmake"}
        )
    for path in code_files:
        try:
            header = "\n".join(path.read_text(encoding="utf-8").splitlines()[:10])
        except UnicodeDecodeError as error:
            raise AssertionError(f"Source file is not UTF-8: {path}") from error
        assert "Copyright (C) 2026 Satoshi Masuko <wakeup1002@gmail.com>" in header, path
        assert "SPDX-License-Identifier: GPL-3.0-or-later" in header, path
    assert "SPDX-License-Identifier: GPL-3.0-or-later" in (
        ROOT / "indi_vixen_wirelessunit.xml.in"
    ).read_text(encoding="utf-8")


def test_automatic_discovery_retries_without_blocking_the_driver():
    source = (ROOT / "src/indi_vixen_wireless.cpp").read_text(encoding="utf-8")
    header = (ROOT / "src/indi_vixen_wireless.hpp").read_text(encoding="utf-8")
    no_response = re.search(
        r"if \(units\.empty\(\)\)\s*\{(.*?)return DiscoveryResult::noResponse;",
        source,
        re.DOTALL,
    )
    assert no_response is not None
    assert "DiscoverySP.s = willRetry ? IPS_BUSY : IPS_ALERT" in no_response.group(1)
    assert "LOGF_ERROR(\"No Wireless Unit responded" in no_response.group(1)
    assert "attempt < maximumAttempts" in no_response.group(1)
    assert "deviceDiscoveryState_ == DeviceDiscoveryState::notScanned ||" in source
    assert "deviceDiscoveryState_ == DeviceDiscoveryState::unavailable" in source
    assert "kAutomaticDiscoveryMaximumAttempts = 4" in source
    assert "kAutomaticDiscoveryRetryMilliseconds = 5000" in source
    assert "IEAddTimer(" in source
    assert "automaticDiscoveryRetryHelper" in source
    assert "Automatic Wireless Unit discovery is already in progress" in source
    assert "automaticDiscoveryAttempt_ < kAutomaticDiscoveryMaximumAttempts" in source
    assert "cancelAutomaticDiscoveryConnect" in source
    connection_busy = re.search(
        r"void VixenWirelessUnit::setAutomaticConnectionBusy\(.*?\n\}",
        source,
        re.DOTALL,
    )
    assert connection_busy is not None
    assert 'findWidgetByName("CONNECT")' in connection_busy.group(0)
    assert "connect->setState(ISS_ON)" in connection_busy.group(0)
    assert 'findWidgetByName("DISCONNECT")' in connection_busy.group(0)
    assert "disconnect->setState(ISS_OFF)" in connection_busy.group(0)
    assert "connection.setState(IPS_BUSY)" in connection_busy.group(0)
    assert 'setConnected(false, IPS_BUSY, "Starting automatic' not in source
    assert 'setConnected(false, IPS_BUSY, "Wireless Unit was not found' not in source
    assert "discoverWirelessUnit(false) == DiscoveryResult::found" in source
    assert "unsigned maximumAttempts = 1" in header
    assert "Run Search again before connecting" not in source
    endpoint_update = re.search(
        r"const bool endpointChanged =.*?if \(endpointChanged\).*?"
        r"DeviceDiscoveryState::manualEndpoint;",
        source,
        re.DOTALL,
    )
    assert endpoint_update is not None
    assert "loadingEndpointConfig_ || isConfigLoading() || loadingDefaults_" in source
    assert "Using manually selected Wireless Unit endpoint" in source


def test_automatic_home_is_opt_in_safe_and_uses_shared_initialization():
    source = (ROOT / "src/indi_vixen_wireless.cpp").read_text(encoding="utf-8")
    header = (ROOT / "src/indi_vixen_wireless.hpp").read_text(encoding="utf-8")
    assert '"WIRELESS_ASSUME_HOME"' in source
    assert '"Assume HOME", MAIN_CONTROL_TAB' in source
    assert '"ON_CONNECT", "On connect", ISS_OFF' in source
    assert "IUSaveConfigSwitch(fp, &AssumeHomeSP);" in source
    assert "bool initializeHome(bool automatic);" in header
    assert "return initializeHome(false);" in source
    assert "if (AssumeHomeS[0].s == ISS_ON)" in source
    assert "initializationState_ == MountInitializationState::uninitialized" in source
    assert "initializeHome(true);" in source
    assert "Failure intentionally does not tear down a valid network" in source
    assert "Disconnect before changing automatic HOME initialization" in source
    assert "LocationNP[LOCATION_LONGITUDE].setValue(indiLongitude);" in source
    for label in (
        '"Device info"', '"GoTo flip dir"', '"Station info"',
        '"Station link"', '"Auto-connect"', '"AP info"',
        '"AP settings"', '"Networks"', '"Search Networks"',
    ):
        assert label in source


def test_indi_messages_do_not_use_runtime_format_strings():
    source = (ROOT / "src/indi_vixen_wireless.cpp").read_text(encoding="utf-8")
    unsafe_legacy_calls = [
        r"IDSet(?:Switch|Number|Text)\([^,]+,\s*(?:message\b|[A-Za-z_!][^;]*\?)",
        r"\.apply\(\s*message\s*\);",
    ]
    for pattern in unsafe_legacy_calls:
        assert re.search(pattern, source) is None
    assert 'IDSetNumber(&GuideRateNP, "%s", message);' in source
    assert 'MovementWESP.apply("%s", message);' in source


def test_package_version_is_shared_with_driver_and_xml():
    cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
    source = (ROOT / "src/indi_vixen_wireless.cpp").read_text(encoding="utf-8")
    template = (ROOT / "indi_vixen_wirelessunit.xml.in").read_text(encoding="utf-8")
    match = re.search(r"project\(indi_vixen_wireless VERSION (\d+\.\d+\.\d+)", cmake)
    assert match is not None
    assert 'VIXEN_DRIVER_VERSION="${PROJECT_VERSION}"' in cmake
    assert "<version>@PROJECT_VERSION@</version>" in template
    assert '<device label="Vixen Wireless Unit" manufacturer="Vixen">' in template
    assert '<driver name="Vixen Wireless Unit">indi_vixen_wirelessunit</driver>' in template
    assert "add_executable(indi_vixen_wirelessunit" in cmake
    assert "configure_file(indi_vixen_wirelessunit.xml.in indi_vixen_wirelessunit.xml @ONLY)" in cmake
    assert 'install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/COPYING"' in cmake
    assert 'docs/indi_vixen_wirelessunit.1' in cmake
    assert '"DRIVER_VERSION", "Driver version", VIXEN_DRIVER_VERSION' in source
    assert match.group(1) in (ROOT / "README.md").read_text(encoding="utf-8")
    package_dir = ROOT / "debian/indi-vixen-wirelessunit"
    if not package_dir.is_dir():
        # build-deb-trixie.sh reproduces the indi-3rdparty layout: the driver
        # source and its flattened package-specific debian directory are
        # siblings below the package build root.
        package_dir = ROOT.parent / "debian"
    assert package_dir.is_dir()
    changelog = (package_dir / "changelog").read_text(encoding="utf-8")
    debian_version = re.search(
        r"^indi-vixen-wirelessunit \((\d+\.\d+\.\d+)-\d+\)", changelog
    )
    assert debian_version is not None
    assert debian_version.group(1) == match.group(1)
    control = (package_dir / "control").read_text(encoding="utf-8")
    assert "Package: indi-vixen-wirelessunit" in control
    assert "Architecture: any" in control
    install_manifest = (package_dir / "indi-vixen-wirelessunit.install").read_text(encoding="utf-8")
    assert "usr/share/man/man1/" in install_manifest
    manual = (ROOT / "docs/indi_vixen_wirelessunit.1").read_text(encoding="utf-8")
    assert ".TH INDI_VIXEN_WIRELESSUNIT 1" in manual
    assert "indiserver" in manual
    assert (package_dir / "source/format").read_text(encoding="utf-8").strip() == "3.0 (quilt)"
    rules = (package_dir / "rules").read_text(encoding="utf-8")
    assert "--sourcedirectory=indi-vixen-wirelessunit" in rules
    build_script = (ROOT / "scripts/build-deb-trixie.sh").read_text(encoding="utf-8")
    assert 'driver_name="indi-vixen-wirelessunit"' in build_script
    assert "-path './build-*'" in build_script
    assert "-path './log'" in build_script
    assert "-path './outputs'" in build_script
    assert "-path '*/__pycache__'" in build_script
    assert "-name '._*'" in build_script
    assert "-name '*.pyc'" in build_script
    assert "--null -T -" in build_script
    assert 'chmod 0644 "${build_root}/debian/${driver_name}.install"' in build_script


def test_current_manual_slew_rates_match_ui_and_docs():
    header = (ROOT / "src/indi_vixen_wireless.hpp").read_text(encoding="utf-8")
    source = (ROOT / "src/indi_vixen_wireless.cpp").read_text(encoding="utf-8")
    english = (ROOT / "README.md").read_text(encoding="utf-8")
    match = re.search(r"slewMultipliers_\s*\{([^}]+)\}", header)
    assert match is not None
    rates = [float(value.strip()) for value in match.group(1).split(",")]
    assert rates == [0.5, 1, 2, 4, 10, 20, 40, 100, 200, 400]
    assert re.search(r"TELESCOPE_CAN_CONTROL_TRACK,\s*10\)", source)
    for rate in ("0.5x", "1x", "2x", "4x", "10x", "20x", "40x", "100x", "200x", "400x"):
        assert rate in english


def test_motion_failures_discard_manual_intent_and_operations_are_interlocked():
    source = (ROOT / "src/indi_vixen_wireless.cpp").read_text(encoding="utf-8")
    status_failure = re.search(
        r"if \(!result\)\s*\{\s*if \(manualMotionActive\(\)\)\s*"
        r"abandonManualMotion\(\"MTSTA failed;.*?return false;",
        source,
        re.DOTALL,
    )
    assert status_failure is not None
    assert 'targetMotionStartAllowed("GoTo")' in source
    assert 'targetMotionStartAllowed("Park")' in source
    assert 'targetMotionStartAllowed("Tracking")' in source
    assert "%s is unavailable while manual motion is active" in source
    assert "%s is unavailable while a pulse guide is active" in source
    assert "Manual motion is unavailable while a pulse guide is active" in source
    assert "Pulse guide is unavailable while manual motion is active" in source
    assert "Pulse guide is unavailable while GoTo is active" in source


def test_disconnect_publishes_transient_mount_state_reset():
    source = (ROOT / "src/indi_vixen_wireless.cpp").read_text(encoding="utf-8")
    reset = re.search(
        r"void VixenWirelessUnit::resetTransientMountStateForDisconnect\(\).*?\n\}",
        source,
        re.DOTALL,
    )
    assert reset is not None
    body = reset.group(0)
    assert "TrackState = SCOPE_IDLE" in body
    assert "synchronizeTransientMountProperties();" in body
    assert "TrackStateSP.apply();" in body
    assert "EqNP.apply();" in body
    assert "AbortSP.apply();" in body
    assert "MovementNSSP.apply();" in body
    assert "MovementWESP.apply();" in body


def test_connected_config_load_does_not_promote_unsent_guide_rates():
    source = (ROOT / "src/indi_vixen_wireless.cpp").read_text(encoding="utf-8")
    guard = source.index(
        "if (isConnected() && (preservingSettingsSourceDuringLoad_ || isConfigLoading() || loadingDefaults_))",
        source.index("if (std::strcmp(name, GuideRateNP.name) == 0)"),
    )
    promote = source.index("guideRa_ = static_cast<float>(GuideRateN[0].value);", guard)
    assert guard < promote
    guarded_block = source[guard:promote]
    assert "GuideRateN[0].value = previousRa" in guarded_block
    assert "GuideRateN[1].value = previousDec" in guarded_block
    assert "return true;" in guarded_block


def test_meridian_validation_is_shared_by_client_simulator_and_driver():
    validation = (ROOT / "src/numeric_validation.hpp").read_text(encoding="utf-8")
    client = (ROOT / "src/client.cpp").read_text(encoding="utf-8")
    simulator = (ROOT / "src/simulator_transport.cpp").read_text(encoding="utf-8")
    driver = (ROOT / "src/indi_vixen_wireless.cpp").read_text(encoding="utf-8")
    assert "std::abs(signedGotoFlipPointDegrees) <= stopPointDegrees" in validation
    assert "numeric_validation::validMeridianSettings" in client
    assert "numeric_validation::validMeridianSettings" in simulator
    assert driver.count("numeric_validation::validMeridianSettings") >= 4
