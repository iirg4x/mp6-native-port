"""Non-interactive desktop QA: hidden rendering, virtual PAD only, muted audio."""
import os
from pathlib import Path

MARKER = '[BACKGROUND-QA] hidden window; scripted input only; controllers/haptics disabled'


def isolate_background(env, executable):
    # Fail before launching an old binary: CREATE_NO_WINDOW only hides its
    # console, not the SDL game window or its physical controller handling.
    if MARKER.encode() not in Path(executable).read_bytes():
        raise ValueError('Executable lacks background QA isolation; rebuild the Windows backend and QA executable')
    env.update(MP6_BACKGROUND_QA='1', SDL_AUDIODRIVER='dummy', SDL_AUDIO_DRIVER='dummy')


def background_process_flags():
    # CREATE_NO_WINDOW is Windows-only. Keep import-time tests portable.
    return 0x08000000 if os.name == 'nt' else 0
