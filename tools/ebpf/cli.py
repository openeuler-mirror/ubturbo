import argparse
from daemon import start_daemon, stop_daemon, show_status
from reporter import print_report


def main():
    parser = argparse.ArgumentParser(description="HugePage memory diagnostic tool")
    subparsers = parser.add_subparsers(dest="command")

    subparsers.add_parser("start", help="Start memdiag daemon")
    subparsers.add_parser("stop", help="Stop memdiag daemon")
    subparsers.add_parser("status", help="Show daemon status")
    subparsers.add_parser("report", help="Show HugePage usage report")

    args = parser.parse_args()

    if args.command == "start":
        start_daemon()
    elif args.command == "stop":
        stop_daemon()
    elif args.command == "status":
        show_status()
    elif args.command == "report":
        print_report()
    else:
        parser.print_help()