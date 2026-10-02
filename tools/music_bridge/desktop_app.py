"""Small Windows desktop shell for the FNK Music Bridge.

The FastAPI bridge remains the source of truth.  This module only adds a
local player and a tray-friendly desktop window around the existing service.
"""

from __future__ import annotations

import threading
import tkinter as tk
from pathlib import Path
from tkinter import messagebox, ttk

import pygame
import pystray
import uvicorn
from PIL import Image, ImageDraw

from tools.music_bridge.app import app, config
from tools.music_bridge.library import MusicLibrary


class BridgeDesktopApp:
    def __init__(self, root: tk.Tk) -> None:
        self.root = root
        self.root.title("FNK Music Bridge")
        self.root.geometry("560x360")
        self.root.minsize(460, 280)
        self.root.protocol("WM_DELETE_WINDOW", self.hide_to_tray)

        self.library = MusicLibrary(config.library_dir)
        self.tracks: list[dict] = []
        self.server: uvicorn.Server | None = None
        self.server_thread: threading.Thread | None = None
        self.tray: pystray.Icon | None = None
        self._restoring = False
        self._closing = False
        self._refresh_after_id: str | None = None
        self._track_signature: tuple[tuple[str, str, int, str], ...] = ()

        self.status = tk.StringVar(value="Готово")
        self._build_ui()
        self._load_tracks()
        self._start_server()
        self._start_tray()
        self._schedule_track_refresh()
        self.root.bind("<Unmap>", self._on_unmap)

    def _build_ui(self) -> None:
        frame = ttk.Frame(self.root, padding=12)
        frame.pack(fill=tk.BOTH, expand=True)

        ttk.Label(frame, text="Готовые треки", font=("Segoe UI", 12, "bold")).pack(
            anchor=tk.W
        )

        list_frame = ttk.Frame(frame)
        list_frame.pack(fill=tk.BOTH, expand=True, pady=(8, 8))
        self.track_list = tk.Listbox(
            list_frame,
            selectmode=tk.SINGLE,
            activestyle="dotbox",
            font=("Segoe UI", 10),
        )
        scrollbar = ttk.Scrollbar(list_frame, orient=tk.VERTICAL, command=self.track_list.yview)
        self.track_list.configure(yscrollcommand=scrollbar.set)
        self.track_list.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)
        scrollbar.pack(side=tk.RIGHT, fill=tk.Y)
        self.track_list.bind("<Double-Button-1>", lambda _: self.play_selected())

        controls = ttk.Frame(frame)
        controls.pack(fill=tk.X)
        ttk.Button(controls, text="Play", command=self.play_selected).pack(
            side=tk.LEFT, padx=(0, 6)
        )
        ttk.Button(controls, text="Stop", command=self.stop_playback).pack(
            side=tk.LEFT, padx=(0, 6)
        )
        ttk.Button(controls, text="\u041e\u0431\u043d\u043e\u0432\u0438\u0442\u044c", command=self._load_tracks).pack(side=tk.RIGHT)
        ttk.Label(frame, textvariable=self.status, foreground="#555").pack(
            anchor=tk.W, pady=(8, 0)
        )

    def _load_tracks(self) -> None:
        # The API worker owns a separate MusicLibrary instance and updates the
        # same persistent index when a generation completes. Reload before
        # reading so newly generated tracks appear in this window.
        self.library.reload()
        selected = self.track_list.curselection() if hasattr(self, "track_list") else ()
        selected_id = self.tracks[selected[0]].get("id") if selected else None
        tracks: list[dict] = []

        for track in self.library.list_tracks():
            track_id = str(track.get("id", ""))
            if not track_id or self.library.audio_path(track_id) is None:
                continue
            tracks.append(track)

        signature = tuple(
            (
                str(track.get("id", "")),
                str(track.get("filename", "")),
                int(track.get("size", 0) or 0),
                str(track.get("ready_at", "")),
            )
            for track in tracks
        )
        self.tracks = tracks
        if signature != self._track_signature and hasattr(self, "track_list"):
            self.track_list.delete(0, tk.END)
            for track in tracks:
                track_id = str(track.get("id", ""))
                title = str(track.get("title") or track.get("filename") or track_id)
                self.track_list.insert(tk.END, title)
            self._track_signature = signature

        if selected_id:
            for index, track in enumerate(self.tracks):
                if track.get("id") == selected_id:
                    self.track_list.selection_set(index)
                    self.track_list.see(index)
                    break
        self.status.set(f"\u0422\u0440\u0435\u043a\u043e\u0432: {len(self.tracks)}")

    def _schedule_track_refresh(self) -> None:
        if not self._closing:
            self._refresh_after_id = self.root.after(3000, self._auto_refresh_tracks)

    def _auto_refresh_tracks(self) -> None:
        self._refresh_after_id = None
        if self._closing:
            return
        try:
            self._load_tracks()
        finally:
            self._schedule_track_refresh()

    def _start_server(self) -> None:
        uvicorn_config = uvicorn.Config(
            app,
            host=config.listen_host,
            port=config.listen_port,
            log_level="info",
            # A --windowed PyInstaller process has no stdout/stderr. Uvicorn's
            # default formatter calls isatty() on that missing stream.
            log_config=None,
        )
        self.server = uvicorn.Server(uvicorn_config)
        self.server_thread = threading.Thread(
            target=self.server.run,
            name="music-bridge-api",
            daemon=True,
        )
        self.server_thread.start()

    def _start_tray(self) -> None:
        self.tray = pystray.Icon(
            "fnk_music_bridge",
            _tray_image(),
            "FNK Music Bridge",
            menu=pystray.Menu(
                pystray.MenuItem("Показать", self._tray_show, default=True),
                pystray.MenuItem("Остановить звук", self._tray_stop),
                pystray.MenuItem("Выход", self._tray_exit),
            ),
        )
        threading.Thread(target=self.tray.run, name="music-bridge-tray", daemon=True).start()

    def _on_unmap(self, _event: tk.Event) -> None:
        if self._closing or self._restoring:
            return
        if self.root.state() == "iconic":
            self.root.after_idle(self.hide_to_tray)

    def hide_to_tray(self) -> None:
        if self._closing:
            return
        self.root.withdraw()
        self.status.set("Скрыто в трее")

    def _tray_show(self, _icon: pystray.Icon, _item: pystray.MenuItem) -> None:
        self.root.after(0, self.show_window)

    def show_window(self) -> None:
        self._restoring = True
        try:
            self.root.deiconify()
            self.root.state("normal")
            self.root.lift()
            self.root.focus_force()
        finally:
            self.root.after(100, lambda: setattr(self, "_restoring", False))

    def _tray_stop(self, _icon: pystray.Icon, _item: pystray.MenuItem) -> None:
        self.root.after(0, self.stop_playback)

    def _tray_exit(self, _icon: pystray.Icon, _item: pystray.MenuItem) -> None:
        self.root.after(0, self.close)

    def play_selected(self) -> None:
        selection = self.track_list.curselection()
        if not selection:
            messagebox.showinfo("FNK Music Bridge", "Выберите готовый трек.")
            return
        path = self.library.audio_path(str(self.tracks[selection[0]].get("id", "")))
        if path is None:
            self.status.set("Файл трека не найден")
            self._load_tracks()
            return
        try:
            pygame.mixer.music.load(str(path))
            pygame.mixer.music.play()
            self.status.set(f"Воспроизводится: {path.name}")
        except (pygame.error, OSError) as error:
            self.status.set(f"Ошибка воспроизведения: {error}")

    def stop_playback(self) -> None:
        try:
            pygame.mixer.music.stop()
            self.status.set("Воспроизведение остановлено")
        except pygame.error as error:
            self.status.set(f"Ошибка остановки: {error}")

    def close(self) -> None:
        if self._closing:
            return
        self._closing = True
        if self._refresh_after_id is not None:
            self.root.after_cancel(self._refresh_after_id)
            self._refresh_after_id = None
        self.stop_playback()
        if self.tray is not None:
            self.tray.stop()
        if self.server is not None:
            self.server.should_exit = True
        if self.server_thread is not None:
            self.server_thread.join(timeout=3)
        self.root.destroy()


def _tray_image() -> Image.Image:
    image = Image.new("RGBA", (64, 64), (35, 95, 155, 255))
    draw = ImageDraw.Draw(image)
    draw.rounded_rectangle((8, 8, 56, 56), radius=10, fill=(245, 245, 245, 255))
    draw.polygon((27, 20, 27, 44, 47, 32), fill=(35, 95, 155, 255))
    return image


def main() -> None:
    pygame.mixer.init()
    root = tk.Tk()
    desktop = BridgeDesktopApp(root)
    root.mainloop()
    desktop.close()


if __name__ == "__main__":
    main()
