"""Small Windows desktop shell for the FNK Music Bridge.

The FastAPI bridge remains the source of truth.  This module only adds a
local player and a tray-friendly desktop window around the existing service.
"""

from __future__ import annotations

import json
import threading
import time
import tkinter as tk
from pathlib import Path
from tkinter import messagebox, ttk

import pygame
import pystray
import requests
import uvicorn
from mutagen.mp3 import MP3
from PIL import Image, ImageDraw

from tools.music_bridge.app import app, config, provider_registry
from tools.music_bridge.library import MusicLibrary


class BridgeDesktopApp:
    def __init__(self, root: tk.Tk) -> None:
        self.root = root
        self.root.title("FNK Music Bridge")
        self.root.geometry("900x600")
        self.root.minsize(720, 460)
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
        self.provider_var = tk.StringVar(value="")
        self.provider_ready_var = tk.StringVar(value="NOT READY")
        self.provider_combo: ttk.Combobox | None = None
        self.provider_status_label: ttk.Label | None = None
        self._provider_labels: dict[str, str] = {}
        self._provider_health_token = 0
        self._current_path: Path | None = None
        self._current_title = ""
        self._player_duration = 0.0
        self._player_offset = 0.0
        self._player_position = 0.0
        self._player_after_id: str | None = None
        self._progress_dragging = False
        self._volume_save_after_id: str | None = None
        self.player_title = tk.StringVar(value="Ничего не воспроизводится")
        self.player_time = tk.StringVar(value="00:00 / 00:00")
        self.progress_value = tk.DoubleVar(value=0.0)
        self.volume_value = tk.DoubleVar(value=float(config.volume_percent))
        self.volume_label = tk.StringVar(value=f"\u0413\u0440\u043e\u043c\u043a\u043e\u0441\u0442\u044c: {config.volume_percent}%")
        self.request_title = tk.StringVar(value="-")
        self.request_style = tk.StringVar(value="-")
        self.request_duration = tk.StringVar(value="-")
        self.request_status = tk.StringVar(value="-")
        self.request_filename = tk.StringVar(value="-")
        self.request_provider = tk.StringVar(value="-")
        self.request_size = tk.StringVar(value="-")
        self.request_date = tk.StringVar(value="-")

        self.status = tk.StringVar(value="Готово")
        self._build_ui()
        self._load_tracks()
        self._start_server()
        self._start_tray()
        self.root.after(500, self._check_provider_health)
        self._schedule_track_refresh()
        self._schedule_player_update()
        self.root.bind("<Unmap>", self._on_unmap)

    def _build_ui(self) -> None:
        frame = ttk.Frame(self.root, padding=12)
        frame.pack(fill=tk.BOTH, expand=True)

        ttk.Label(frame, text="Готовые треки", font=("Segoe UI", 12, "bold")).pack(
            anchor=tk.W
        )

        provider_row = ttk.Frame(frame)
        provider_row.pack(fill=tk.X, pady=(8, 0))
        ttk.Label(provider_row, text="Провайдер:").pack(side=tk.LEFT)
        self.provider_combo = ttk.Combobox(
            provider_row,
            textvariable=self.provider_var,
            state="readonly",
            width=34,
        )
        self.provider_combo.pack(side=tk.LEFT, padx=(8, 0))
        self.provider_combo.bind("<<ComboboxSelected>>", self._on_provider_selected)
        self.provider_status_label = ttk.Label(
            provider_row,
            textvariable=self.provider_ready_var,
            foreground="#c62828",
            font=("Segoe UI", 9, "bold"),
        )
        self.provider_status_label.pack(side=tk.LEFT, padx=(14, 0))

        content = ttk.Frame(frame)
        content.pack(fill=tk.BOTH, expand=True, pady=(8, 8))

        request_frame = ttk.LabelFrame(content, text="\u0412\u044b\u0431\u0440\u0430\u043d\u043d\u044b\u0439 \u0442\u0440\u0435\u043a", padding=10)
        request_frame.pack(side=tk.RIGHT, fill=tk.Y, padx=(12, 0))

        for label, variable in (
            ("\u041d\u0430\u0437\u0432\u0430\u043d\u0438\u0435", self.request_title),
            ("\u0424\u0430\u0439\u043b", self.request_filename),
            ("\u041f\u0440\u043e\u0432\u0430\u0439\u0434\u0435\u0440", self.request_provider),
            ("\u0421\u0442\u0438\u043b\u044c", self.request_style),
            ("\u0414\u043b\u0438\u0442\u0435\u043b\u044c\u043d\u043e\u0441\u0442\u044c", self.request_duration),
            ("\u0420\u0430\u0437\u043c\u0435\u0440", self.request_size),
            ("\u0414\u0430\u0442\u0430", self.request_date),
            ("\u0421\u0442\u0430\u0442\u0443\u0441", self.request_status),
        ):
            ttk.Label(request_frame, text=label, font=("Segoe UI", 9, "bold")).pack(
                anchor=tk.W
            )
            ttk.Label(request_frame, textvariable=variable, wraplength=260).pack(
                anchor=tk.W, fill=tk.X, pady=(0, 8)
            )

        ttk.Label(request_frame, text="\u0422\u0435\u043a\u0441\u0442", font=("Segoe UI", 9, "bold")).pack(
            anchor=tk.W
        )
        self.request_lyrics = tk.Text(
            request_frame,
            width=32,
            height=15,
            wrap=tk.WORD,
            font=("Segoe UI", 9),
            state=tk.DISABLED,
        )
        self.request_lyrics.pack(fill=tk.BOTH, expand=True)

        left_frame = ttk.Frame(content)
        left_frame.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)

        list_frame = ttk.Frame(left_frame)
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
        self.track_list.bind("<<ListboxSelect>>", lambda _: self._show_selected_track())
        self.track_list.bind("<Double-Button-1>", lambda _: self.play_selected())

        player_frame = ttk.LabelFrame(left_frame, text="Проигрыватель", padding=10)
        player_frame.pack(fill=tk.X, pady=(8, 0))
        ttk.Label(
            player_frame,
            textvariable=self.player_title,
            font=("Segoe UI", 10, "bold"),
        ).pack(anchor=tk.W, fill=tk.X)
        self.progress_scale = ttk.Scale(
            player_frame,
            from_=0.0,
            to=1.0,
            variable=self.progress_value,
            command=self._on_progress_drag,
        )
        self.progress_scale.pack(fill=tk.X, pady=(10, 2))
        self.progress_scale.bind("<ButtonRelease-1>", self._on_progress_release)

        player_controls = ttk.Frame(player_frame)
        player_controls.pack(fill=tk.X)
        ttk.Button(player_controls, text="Play", command=self.play_selected).pack(
            side=tk.LEFT, padx=(0, 6)
        )
        ttk.Button(player_controls, text="Stop", command=self.stop_playback).pack(
            side=tk.LEFT, padx=(0, 6)
        )
        ttk.Label(player_controls, textvariable=self.player_time).pack(side=tk.RIGHT)

        volume_row = ttk.Frame(player_frame)
        volume_row.pack(fill=tk.X, pady=(8, 0))
        ttk.Label(volume_row, textvariable=self.volume_label).pack(side=tk.LEFT)
        ttk.Scale(
            volume_row,
            from_=0.0,
            to=100.0,
            variable=self.volume_value,
            command=self._on_volume_change,
        ).pack(side=tk.LEFT, fill=tk.X, expand=True, padx=(10, 0))

        controls = ttk.Frame(left_frame)
        controls.pack(fill=tk.X, pady=(6, 0))
        ttk.Button(controls, text="\u041e\u0431\u043d\u043e\u0432\u0438\u0442\u044c", command=self._load_tracks).pack(side=tk.RIGHT)
        ttk.Label(left_frame, textvariable=self.status, foreground="#555").pack(
            anchor=tk.W, pady=(8, 0)
        )

    @staticmethod
    def _format_time(seconds: float) -> str:
        seconds = max(0, int(seconds))
        minutes, remainder = divmod(seconds, 60)
        hours, minutes = divmod(minutes, 60)
        return f"{hours}:{minutes:02d}:{remainder:02d}" if hours else f"{minutes:02d}:{remainder:02d}"

    def _set_player_time(self, position: float) -> None:
        self.player_time.set(
            f"{self._format_time(position)} / {self._format_time(self._player_duration)}"
        )

    def _schedule_player_update(self) -> None:
        if not self._closing:
            self._player_after_id = self.root.after(250, self._update_player)

    def _update_player(self) -> None:
        self._player_after_id = None
        if self._current_path is not None and not self._progress_dragging:
            busy = pygame.mixer.music.get_busy()
            position = self._player_offset
            if busy:
                position += max(0.0, pygame.mixer.music.get_pos() / 1000.0)
                if self._player_duration:
                    position = min(position, self._player_duration)
                self._player_position = position
                self.progress_value.set(position)
                self._set_player_time(position)
            elif self._player_duration and self._player_position >= self._player_duration - 0.5:
                self.progress_value.set(self._player_duration)
                self._set_player_time(self._player_duration)
                self.player_title.set(f"Завершено: {self._current_title}")
        self._schedule_player_update()

    def _on_progress_drag(self, value: str) -> None:
        if self._current_path is None or not self._player_duration:
            return
        self._progress_dragging = True
        self._set_player_time(float(value))

    def _on_progress_release(self, _event: tk.Event) -> None:
        if not self._progress_dragging or self._current_path is None:
            return
        self._progress_dragging = False
        self._seek_to(float(self.progress_value.get()))

    def _seek_to(self, position: float) -> None:
        if self._current_path is None or not self._current_path.is_file():
            return
        position = max(0.0, min(float(position), self._player_duration or float("inf")))
        try:
            pygame.mixer.music.play(start=position)
            self._player_offset = position
            self._player_position = position
            self.player_title.set(f"Воспроизводится: {self._current_title}")
            self._set_player_time(position)
        except pygame.error as error:
            self.status.set(f"Ошибка перемотки: {error}")

    def _on_volume_change(self, value: str) -> None:
        try:
            volume = max(0.0, min(100.0, float(value)))
        except (TypeError, ValueError):
            return
        self.volume_value.set(volume)
        self.volume_label.set(f"\u0413\u0440\u043e\u043c\u043a\u043e\u0441\u0442\u044c: {int(round(volume))}%")
        try:
            pygame.mixer.music.set_volume(volume / 100.0)
        except pygame.error:
            pass
        if self._volume_save_after_id is not None:
            self.root.after_cancel(self._volume_save_after_id)
        self._volume_save_after_id = self.root.after(350, self._save_volume_setting)

    def _save_volume_setting(self) -> None:
        self._volume_save_after_id = None
        try:
            config.save_volume_percent(int(round(self.volume_value.get())))
        except (OSError, TypeError, ValueError, json.JSONDecodeError):
            pass

    @staticmethod
    def _format_size(size: object) -> str:
        try:
            value = max(0, int(size or 0))
        except (TypeError, ValueError):
            return "-"
        units = ("B", "KB", "MB", "GB")
        amount = float(value)
        for unit in units:
            if amount < 1024 or unit == units[-1]:
                return f"{amount:.1f} {unit}" if unit != "B" else f"{int(amount)} {unit}"
            amount /= 1024
        return "-"

    def _show_selected_track(self) -> None:
        """Show metadata for the row selected in the track list."""
        selection = self.track_list.curselection()
        track = self.tracks[selection[0]] if selection and selection[0] < len(self.tracks) else None
        full = self.library.get(str(track.get("id", ""))) if track else None
        metadata = full or track or {}
        title = str(metadata.get("title") or metadata.get("filename") or "-")
        self.request_title.set(title)
        self.request_filename.set(str(metadata.get("filename") or "-"))
        self.request_provider.set(str(metadata.get("provider") or metadata.get("source") or "-"))
        self.request_style.set(str(metadata.get("style") or metadata.get("artist") or "-"))
        duration = metadata.get("duration") or metadata.get("duration_seconds")
        try:
            duration_text = self._format_time(float(duration)) if duration else "-"
        except (TypeError, ValueError):
            duration_text = "-"
        self.request_duration.set(duration_text)
        self.request_size.set(self._format_size(metadata.get("size")))
        self.request_date.set(str(metadata.get("ready_at") or metadata.get("created_at") or "-"))
        status = str(metadata.get("status") or ("READY" if track else "-"))
        self.request_status.set(status)
        lyrics = str(metadata.get("lyrics") or "")
        if not lyrics and metadata.get("artist"):
            lyrics = f"Artist: {metadata.get('artist')}"
            if metadata.get("album"):
                lyrics += f"\nAlbum: {metadata.get('album')}"
        self.request_lyrics.configure(state=tk.NORMAL)
        self.request_lyrics.delete("1.0", tk.END)
        self.request_lyrics.insert("1.0", lyrics)
        self.request_lyrics.configure(state=tk.DISABLED)

    def _refresh_provider_options(self) -> None:
        if self.provider_combo is None:
            return
        provider_registry.refresh()
        labels: dict[str, str] = {}
        for provider in provider_registry.all():
            labels[f"{provider.name} ({provider.provider_id})"] = provider.provider_id
        self._provider_labels = labels
        values = list(labels)
        self.provider_combo.configure(values=values)
        active = provider_registry.active_id
        self.provider_var.set(next((label for label, item in labels.items() if item == active), ""))

    def _on_provider_selected(self, _event: tk.Event) -> None:
        provider_id = self._provider_labels.get(self.provider_var.get())
        if not provider_id:
            return
        try:
            provider = provider_registry.select(provider_id)
            self.status.set(f"Провайдер: {provider.name}")
            self._set_provider_ready(False)
            self._check_provider_health()
        except ValueError as error:
            self.status.set(f"Ошибка провайдера: {error}")

    def _set_provider_ready(self, ready: bool) -> None:
        self.provider_ready_var.set("READY" if ready else "NOT READY")
        if self.provider_status_label is not None:
            self.provider_status_label.configure(foreground="#2e7d32" if ready else "#c62828")

    def _check_provider_health(self) -> None:
        """Check the selected provider without blocking Tk's UI thread."""
        provider_id = provider_registry.active_id
        self._provider_health_token += 1
        token = self._provider_health_token
        self._set_provider_ready(False)

        def worker() -> None:
            ready = False
            try:
                response = requests.get(
                    f"http://{config.listen_host if config.listen_host not in {'0.0.0.0', '::'} else '127.0.0.1'}:{config.listen_port}/health",
                    timeout=10,
                )
                payload = response.json()
                ready = bool(response.ok and payload.get("ok") and payload.get("active_provider") == provider_id)
            except (OSError, requests.RequestException, ValueError, TypeError):
                ready = False

            def apply_result() -> None:
                if token == self._provider_health_token and provider_registry.active_id == provider_id:
                    self._set_provider_ready(ready)
                    if not self._closing:
                        self.root.after(15000, self._check_provider_health)

            self.root.after(0, apply_result)

        threading.Thread(target=worker, name="provider-health", daemon=True).start()

    def _load_tracks(self) -> None:
        # The API worker owns a separate MusicLibrary instance and updates the
        # same persistent index when a generation completes. Reload before
        # reading so newly generated tracks appear in this window.
        self.library.reload()
        self._refresh_provider_options()
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
        self._show_selected_track()
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
            duration = float(MP3(path).info.length)
            pygame.mixer.music.load(str(path))
            pygame.mixer.music.set_volume(float(self.volume_value.get()) / 100.0)
            pygame.mixer.music.play()
            self._current_path = path
            self._current_title = str(self.tracks[selection[0]].get("title") or path.name)
            self._player_duration = max(0.0, duration)
            self._player_offset = 0.0
            self._player_position = 0.0
            self.progress_scale.configure(to=max(1.0, self._player_duration))
            self.progress_value.set(0.0)
            self._set_player_time(0.0)
            self.player_title.set(f"Воспроизводится: {self._current_title}")
            self.status.set(f"Воспроизводится: {path.name}")
        except (pygame.error, OSError, ValueError) as error:
            self.status.set(f"Ошибка воспроизведения: {error}")

    def stop_playback(self) -> None:
        try:
            pygame.mixer.music.stop()
            self._player_offset = 0.0
            self._player_position = 0.0
            self.progress_value.set(0.0)
            self._set_player_time(0.0)
            if self._current_title:
                self.player_title.set(f"Остановлено: {self._current_title}")
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
        if self._player_after_id is not None:
            self.root.after_cancel(self._player_after_id)
            self._player_after_id = None
        if self._volume_save_after_id is not None:
            self.root.after_cancel(self._volume_save_after_id)
            self._save_volume_setting()
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
