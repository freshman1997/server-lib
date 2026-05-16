import argparse
import sys
from pathlib import Path

from impacket import smb3structs
from impacket.smbconnection import SMBConnection


def parse_args():
    parser = argparse.ArgumentParser(description="SMB NAS smoke client using impacket")
    parser.add_argument("--host", required=True)
    parser.add_argument("--port", required=True, type=int)
    parser.add_argument("--share", required=True)
    parser.add_argument("--domain", default="WORKGROUP")
    parser.add_argument("--user", required=True)
    parser.add_argument("--password", required=True)
    parser.add_argument("--signing", default="default", choices=("default", "auto", "required"))
    parser.add_argument("--local-file", required=True)
    parser.add_argument("--downloaded-file", required=True)
    parser.add_argument("--remote-file", required=True)
    parser.add_argument("--renamed-file", required=True)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    conn = SMBConnection(
        remoteName=args.host,
        remoteHost=args.host,
        sess_port=args.port,
        timeout=10,
        preferredDialect=smb3structs.SMB2_DIALECT_21,
    )
    try:
        conn.login(args.user, args.password, args.domain)
        print(f"INFO: dialect=0x{conn.getDialect():04x} signing_required={conn.isSigningRequired()}")
        if args.signing == "required" and not conn.isSigningRequired():
            print("FAIL: server did not require SMB signing", file=sys.stderr)
            return 1
        if args.signing == "required":
            inner = conn._SMBConnection
            inner._Session["SigningActivated"] = True
            if inner._Connection["SequenceWindow"] <= 2:
                inner._Connection["SequenceWindow"] = 3

        local_path = Path(args.local_file)
        downloaded_path = Path(args.downloaded_file)

        with local_path.open("rb") as source:
            conn.putFile(args.share, args.remote_file, source.read)

        with downloaded_path.open("wb") as target:
            conn.getFile(args.share, args.remote_file, target.write)

        if local_path.read_bytes() != downloaded_path.read_bytes():
            print("FAIL: downloaded file content mismatch", file=sys.stderr)
            return 1

        conn.rename(args.share, args.remote_file, args.renamed_file)
        conn.deleteFile(args.share, args.renamed_file)
        conn.listPath(args.share, "*")
        print("PASS: impacket SMB NAS smoke completed")
        return 0
    finally:
        try:
            conn.close()
        except Exception:
            pass


if __name__ == "__main__":
    raise SystemExit(main())
