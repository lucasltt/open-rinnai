"""Create unique local credentials without overwriting or printing them."""
from pathlib import Path
import argparse
import base64
import secrets

ROOT = Path(__file__).resolve().parent.parent


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=ROOT / "esphome/secrets.yaml")
    args = parser.parse_args()
    destination = args.output.resolve()
    values = {
        "sniffer_ap_password": secrets.token_urlsafe(18),
        "api_encryption_key": base64.b64encode(secrets.token_bytes(32)).decode("ascii"),
        "ota_password": secrets.token_urlsafe(24),
        "web_username": "admin",
        "web_password": secrets.token_urlsafe(24),
    }
    text = "# Local credentials. Never commit this file.\n"
    text += "\n".join(f'{key}: "{value}"' for key, value in values.items()) + "\n"
    destination.parent.mkdir(parents=True, exist_ok=True)
    try:
        with destination.open("x", encoding="utf-8", newline="\n") as handle:
            handle.write(text)
    except FileExistsError:
        raise SystemExit("O arquivo já existe e foi preservado. Edite-o manualmente se necessário.")
    print(f"Credenciais criadas em {destination}. Consulte o arquivo localmente; não o publique.")


if __name__ == "__main__":
    main()
