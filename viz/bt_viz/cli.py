"""Command-line entry point: render the backtest panels, optionally watching.

    bt-viz reports/series_sample.csv                 # -> reports/series_sample.png
    bt-viz reports/series.csv -o out.png --show      # also open a window
    bt-viz reports/series.csv --trades market_data/trades.csv
    bt-viz reports/series.csv --watch                # re-render whenever it changes
"""

from __future__ import annotations

import argparse
import sys
import time
from pathlib import Path

from bt_viz.plot import load_series, load_trades, render


def _default_out(series_path: Path) -> Path:
    return series_path.with_suffix(".png")


def _render_once(args: argparse.Namespace) -> Path:
    series = load_series(args.series)
    trades = load_trades(args.trades) if args.trades else None
    out = Path(args.out) if args.out else _default_out(Path(args.series))
    title = args.title or Path(args.series).stem
    import matplotlib.pyplot as plt

    fig = render(series, out=out, trades=trades, title=title, max_points=args.max_points)
    if args.show:
        plt.show()
    plt.close(fig)
    print(f"wrote {out} ({len(series)} rows)")
    return out


def _watch(args: argparse.Namespace) -> int:
    """Re-render on every change to the series file (the 'watcher service')."""
    try:
        from watchdog.events import FileSystemEventHandler
        from watchdog.observers import Observer
    except ImportError:
        return _watch_poll(args)

    target = Path(args.series).resolve()

    class _Handler(FileSystemEventHandler):
        def on_modified(self, event):
            if Path(event.src_path).resolve() == target:
                _safe_render(args)

        on_created = on_modified

    _safe_render(args)
    observer = Observer()
    observer.schedule(_Handler(), str(target.parent), recursive=False)
    observer.start()
    print(f"watching {target} (Ctrl-C to stop)")
    try:
        while True:
            time.sleep(1)
    except KeyboardInterrupt:
        pass
    finally:
        observer.stop()
        observer.join()
    return 0


def _watch_poll(args: argparse.Namespace) -> int:
    """Fallback watcher when watchdog isn't installed: poll the file mtime."""
    target = Path(args.series)
    print(f"watching {target} via mtime poll (Ctrl-C to stop)")
    last = -1.0
    try:
        while True:
            if target.exists():
                mtime = target.stat().st_mtime
                if mtime != last:
                    last = mtime
                    _safe_render(args)
            time.sleep(1)
    except KeyboardInterrupt:
        pass
    return 0


def _safe_render(args: argparse.Namespace) -> None:
    try:
        _render_once(args)
    except Exception as exc:  # keep the watcher alive across transient read errors
        print(f"render failed: {exc}", file=sys.stderr)


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(description="Plot cmf backtesting-engine time series.")
    p.add_argument("series", help="path to the engine series CSV (series_csv in the config)")
    p.add_argument("-o", "--out", help="output PNG path (default: alongside the CSV)")
    p.add_argument("--trades", help="optional raw trades CSV to overlay market volume")
    p.add_argument("--title", help="figure title (default: the CSV stem)")
    p.add_argument(
        "--max-points",
        type=int,
        default=3000,
        help="bucket a long series down to ~N points before plotting (0 = no limit)",
    )
    p.add_argument("--show", action="store_true", help="open an interactive window too")
    p.add_argument("--watch", action="store_true", help="re-render whenever the CSV changes")
    args = p.parse_args(argv)

    if args.show:
        import matplotlib

        matplotlib.use("TkAgg", force=True)

    if args.watch:
        return _watch(args)
    _render_once(args)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
