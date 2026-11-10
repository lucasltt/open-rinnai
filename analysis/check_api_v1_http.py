"""Validate deployed API v1 with reads and deliberately invalid writes only.

No valid control, cancellation, configuration or restart command is sent.
Run manually after OTA confirmation; --help and --validate-local never connect.
"""
from __future__ import annotations

import argparse
from copy import deepcopy
from datetime import datetime, timezone
import hashlib
import json
import math
from pathlib import Path

import jsonschema
import requests


ROOT = Path(__file__).resolve().parent.parent
OPENAPI = ROOT / "esphome/api/openapi.json"


def json_schema(document: dict) -> dict:
    """Translate OAS 3.0 nullable for validation with a JSON Schema validator."""
    def convert(value):
        if isinstance(value, list):
            return [convert(item) for item in value]
        if not isinstance(value, dict):
            return value
        result = {key: convert(item) for key, item in value.items() if key != "nullable"}
        if value.get("nullable"):
            if "type" in result:
                result["type"] = [result["type"], "null"]
            if "enum" in result and None not in result["enum"]:
                result["enum"].append(None)
        return result
    return convert(deepcopy(document))


def validate(value, schema, document):
    wrapped = dict(schema)
    wrapped["components"] = document["components"]
    jsonschema.Draft7Validator(wrapped).validate(value)


def accounting(value: dict, settings: dict, *, separated: bool = True):
    """Verify configured tariffs, unknown costs and the combined subtotal."""
    gas_tariff = settings["gas_tariff"]
    water_tariff = settings["water_tariff"]
    assert isinstance(value["cost_complete"], bool)
    assert value["cost_complete"] == (gas_tariff > 0 and water_tariff > 0)
    if not separated:
        if not gas_tariff and not water_tariff:
            assert value["cost"] is None
        return
    gas = value["gas_cost"]
    water = value["water_cost"]
    if gas_tariff > 0:
        assert isinstance(gas, (int, float)) and not isinstance(gas, bool)
        assert math.isclose(gas, value["gas"] * gas_tariff, rel_tol=2e-5,
                            abs_tol=max(.002, gas_tariff * .0000051))
    else:
        assert gas is None
    if water_tariff > 0:
        assert isinstance(water, (int, float)) and not isinstance(water, bool)
        assert math.isclose(water, value["water_l"] / 1000 * water_tariff,
                            rel_tol=2e-5, abs_tol=max(.002, water_tariff * .00000051))
    else:
        assert water is None
    available = [part for part in (gas, water) if part is not None]
    if available:
        assert math.isclose(value["cost"], sum(available), rel_tol=2e-5, abs_tol=.002)
    else:
        assert value["cost"] is None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--base", default="http://192.168.1.50")
    parser.add_argument("--expected-version", default="1.2.0")
    parser.add_argument("--validate-local", action="store_true",
                        help="Validate local OpenAPI/examples; no network access")
    args = parser.parse_args()
    contract = json.loads(OPENAPI.read_text(encoding="utf-8"))
    document = json_schema(contract)
    from embed_api_assets import validate_document
    validate_document(contract)
    examples = 0
    for path in contract["paths"].values():
        for operation in path.values():
            for media in operation.get("requestBody", {}).get("content", {}).values():
                if "example" in media:
                    validate(media["example"], json_schema(media["schema"]), document)
                    examples += 1
    request_examples = {
        "TemperatureRequest": {"target": 38},
        "FavoriteRequest": {"slot": 1},
        "TariffRequest": {"gas_type": "GN", "gas_tariff": 8.5, "water_tariff": 12},
        "PreferencesRequest": {
            "favorites": [{"name": "Banho", "temp_c": 38},
                          {"name": "Conforto", "temp_c": 39},
                          {"name": "Cozinha", "temp_c": 40}],
            "alerts": {"long_use_enabled": True, "flow_enabled": True,
                       "error_enabled": True, "offline_enabled": True,
                       "long_use_min": 15, "continuous_flow_min": 20,
                       "offline_seconds": 30}},
        "CancelRequest": {},
        "RestartRequest": {"confirm": True},
    }
    for name, value in request_examples.items():
        validate(value, {"$ref": f"#/components/schemas/{name}"}, document)
        examples += 1
    if args.validate_local:
        print(json.dumps({"passed": True, "network_access": False,
                          "paths": len(contract["paths"]), "request_examples": examples}))
        return

    report = {"recorded_utc": datetime.now(timezone.utc).isoformat(),
              "base": args.base, "expected_version": args.expected_version,
              "valid_write_commands_sent": False, "checks": {}}
    session = requests.Session()
    session.trust_env = False
    base = args.base.rstrip("/")

    def check(name, path, expected=200, *, method="GET", **kwargs):
        response = session.request(method, base + path, timeout=15, **kwargs)
        data = response.json()
        report["checks"][name] = {"path": path, "method": method,
                                   "status": response.status_code, "data": data}
        statuses = expected if isinstance(expected, tuple) else (expected,)
        assert response.status_code in statuses, (name, response.status_code, data)
        assert "WWW-Authenticate" not in response.headers
        assert response.headers.get("Content-Type", "").startswith("application/json")
        return data, response.status_code

    def resource(name, path, schema, expected=200):
        data, status = check(name, "/api/v1" + path, expected)
        validate(data, {"$ref": f"#/components/schemas/{schema}"}, document)
        return data, status

    try:
        for name, path in (("index", ""), ("index_slash", "/"),
                           ("capabilities", "/capabilities")):
            caps, _ = resource(name, path, "Capabilities")
            assert caps["api_version"] == "1"
            assert caps["firmware_version"] == args.expected_version
            assert caps["limits"]["sessions"] == 24
            assert caps["limits"]["history_points"] == 720
            assert caps["limits"]["restart_min_uptime_s"] == 75
            assert caps["capabilities"]["raw_serial"] is False

        state, _ = resource("state", "/state", "State")
        assert state["version"] == args.expected_version
        assert state["api_version"] == "1"
        settings, _ = resource("settings", "/settings", "Settings")
        assert settings == state["settings"]
        for name in ("session", "totals"):
            accounting(state[name], settings)
        for name in ("today", "month", "unassigned"):
            accounting(state["periods"][name], settings)

        telemetry, _ = resource("telemetry", "/telemetry", "Telemetry")
        if telemetry["readings"]["setpoint"]["stale"]:
            assert telemetry["setpoint_c"] is None
        control, _ = resource("control", "/control", "Control")
        before_command = (control["boot_id"], control["command_id"], control["result"])
        history, _ = resource("history", "/history", "History")
        assert history["sample_columns"] == ["age_s", "inlet_c", "outlet_c", "flow_l_min", "power_kw", "heating"]
        for item in history["sessions"]:
            if item["started_at_epoch_s"] is not None:
                assert item["elapsed_s"] == item["ended_at_epoch_s"] - item["started_at_epoch_s"]
        periods, _ = resource("periods", "/periods", "Periods")
        for item in periods["days"] + periods["months"]:
            accounting(item, settings)
        diagnostics, _ = resource("diagnostics", "/diagnostics", "Diagnostics")
        assert len(diagnostics["firmware_elf_sha256"]) == 64
        health, health_status = resource("health", "/health", "Health", (200, 503))
        assert health["version"] == args.expected_version
        assert (health_status == 200) == (health["online"] and health["storage_ok"])
        assert health["status"] == ("ok" if health_status == 200 else "degraded")
        assert health_status == 200, "Device health is degraded; inspect evidence before publishing"

        # Every write below is rejected before it can reach a work queue.
        invalid = [
            ("bad_temperature", "/temperature", {"target": 99}),
            ("bad_favorite", "/favorite", {"slot": 0}),
            ("bad_settings", "/settings", {"gas_type": "GN"}),
            ("bad_preferences", "/preferences", {"favorites": []}),
            ("bad_cancel", "/control/cancel", {"unexpected": True}),
            ("bad_restart", "/device/restart", {"confirm": False}),
        ]
        for name, path, body in invalid:
            error, _ = check(name, "/api/v1" + path, 400, method="POST", json=body)
            validate(error, {"$ref": "#/components/schemas/Error"}, document)
            assert error["code"] == "invalid_request"
        for name, kwargs, status, code in [
            ("foreign_origin", {"json": {"target": 99}, "headers": {"Origin": "http://example.invalid"}}, 403, "origin_not_allowed"),
            ("wrong_mime", {"data": '{"target":99}', "headers": {"Content-Type": "text/plain"}}, 415, "unsupported_media_type"),
            ("malformed_json", {"data": '{"target":', "headers": {"Content-Type": "application/json"}}, 400, "invalid_request"),
        ]:
            error, _ = check(name, "/api/v1/temperature", status, method="POST", **kwargs)
            assert error["code"] == code
        error, _ = check("unknown_endpoint", "/api/v1/unknown", 404)
        assert error["code"] == "not_found"
        error, _ = check("wrong_method", "/api/v1/temperature", 405)
        assert error["code"] == "method_not_allowed"

        for name in ("state", "history", "periods", "diagnostics"):
            legacy, _ = check("legacy_" + name, "/rinnai/api/" + name)
            if name == "state":
                assert legacy["version"] == args.expected_version
                accounting(legacy["totals"], legacy["settings"])
        for path in ("/api", "/api/", "/api/docs"):
            response = session.get(base + path, timeout=15)
            expected = (ROOT / "esphome/api/index.html").read_bytes()
            assert response.status_code == 200 and response.content == expected
            assert response.headers.get("Content-Type", "").startswith("text/html")
            report["checks"][path] = {"status": 200, "bytes": len(expected),
                                        "sha256": hashlib.sha256(expected).hexdigest()}
        response = session.get(base + "/api/openapi.json", timeout=15)
        assert response.status_code == 200 and response.json() == contract
        report["checks"]["/api/openapi.json"] = {"status": 200,
            "bytes": len(response.content), "sha256": hashlib.sha256(response.content).hexdigest()}
        after, _ = resource("control_after_invalid_writes", "/control", "Control")
        assert (after["boot_id"], after["command_id"], after["result"]) == before_command
        assert after["cancel_requested"] == control["cancel_requested"]
        report["passed"] = True
    except Exception as error:
        report["passed"] = False
        report["failure"] = str(error)
        raise
    finally:
        destination = ROOT / "analysis/diagnostics/api_v1_http_check.json"
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
        print(json.dumps({"passed": report.get("passed", False), "checks": len(report["checks"]),
                          "valid_write_commands_sent": False, "evidence": str(destination)}))


if __name__ == "__main__":
    main()
