"""Explicitly supported recovered overlays; never enable partial owners by glob."""
from pathlib import Path

try:
    from .native_namespace import namespace_flags as shared_namespace_flags
except ImportError:  # build.py imports tools modules as top-level modules.
    from native_namespace import namespace_flags as shared_namespace_flags

# runtime.c is the PPC compiler runtime, supplied by the native toolchain.
MODULES = {
    'm602Dll': ('prolog', 'sequence', 'support', 'stage', 'effect', 'player', 'ui'),
    'm608dll': ('prolog', 'sequence', 'setup', 'helpers', 'score', 'sprite', 'ai', 'data'),
    'm609dll': ('startup', 'course', 'intro', 'model_setup', 'player', 'texture_matrix'),
    'm612dll': ('prolog', 'm612', 'tables'),
    'm613Dll': ('sequence', 'camera', 'player', 'geometry', 'bitmap', 'progress', 'easing'),
    'm617dll': ('prolog', 'text_00118', 'text_058DC', 'text_093D0', 'text_0D3CC', 'text_0E1F0', 'text_187C8'),
    'm618dll': ('prolog', 'sequence', 'game'),
    'm619Dll': ('scene', 'motion_callbacks', 'particle_setup', 'player'),
    'm620Dll': ('text_00000', 'text_0117C', 'text_49F8', 'text_64A8', 'text_0B7D4', 'text_0ED3C', 'text_F180'),
    'm621dll': ('prolog', 'm621', 'gameplay'),
    'm625Dll': ('text_0', 'text_15C8', 'text_296C', 'text_11040', 'text_13318', 'text_13C04', 'score_box'),
    'm626Dll': ('text_00000', 'text_0052C', 'text_04284', 'collision', 'effects', 'player', 'scenery', 'support'),
    'm627dll': ('startup', 'gameplay'),
    'm629Dll': ('m629',),
    'm630dll': ('prolog', 'sequence', 'game', 'player'),
    'm631Dll': ('actor', 'game', 'utilities'),
    'm632dll': ('prolog', 'sequence', 'game', 'grid', 'qsort'),
    'm633dll': ('prolog', 'early', 'input_model', 'audio_helper', 'setup',
                'model_id', 'middle', 'angle', 'camera'),
    'm635dll': ('prolog', 'm635', 'logic', 'graphics'),
    'm636dll': ('src/REL/executor.c', 'ai', 'camera', 'main', 'pattern', 'score', 'sequence'),
    'm637dll': ('prolog', 'sequence', 'game'),
    'm638Dll': ('prolog', 'game', 'player', 'scheduler'),
    'm640dll': ('prolog', 'sequence', 'game'),
    'm642dll': ('startup', 'main', 'text_07878'),
    'm643DLL': ('prolog', 'startup', 'game', 'math'),
    'm644dll': ('prolog', 'menu', 'scene', 'camera', 'stage', 'routes', 'player'),
    'm646Dll': ('actors', 'character_motion', 'collision', 'model_attachments',
                'model_shadows', 'score_display', 'sequence', 'spatial_audio', 'target_selection'),
    'm647dll': ('text_00000', 'text_00A74', 'text_064B8', 'text_AFB0'),
    'm649Dll': ('prolog', 'main', 'camera', 'player', 'prop', 'effect', 'score'),
    'm650dll': ('prolog', 'sequence', 'game', 'obstacle'),
    'm651dll': ('prolog', 'm651', 'players'),
    'm656DLL': ('prolog', 'sequence', 'matrix', 'collision', 'game'),
    'm657Dll': ('m657', 'arena', 'player', 'score', 'com', 'hud'),
    'm658dll': ('main', 'gameplay', 'math_camera'),
    'm659Dll': ('camera', 'collision', 'com', 'globals', 'objects', 'player', 'scene'),
    'm660dll': ('text_00000', 'text_06BF8', 'text_0A3E4', 'text_0D29C', 'text_0E6F4'),
    'm662dll': ('main', 'game', 'puzzle', 'ending'),
    'm664dll': ('setup', 'game'),
    'm665dll': ('main', 'scene', 'player', 'opponent'),
    'm666dll': ('text_000A0', 'text_008BC'),
    'm667dll': ('startup', 'map_players_effects', 'particles_tracks_materials', 'model_sound_description'),
    'm668DLL': ('callbacks', 'data', 'geometry', 'math', 'startup'),
    'm675Dll': ('camera', 'collision', 'effects', 'gameplay', 'main', 'player', 'score'),
    'm674Dll': ('main', 'scene', 'player', 'collision', 'effects', 'score', 'targets'),
    'm676Dll': ('main', 'camera_audio', 'collision', 'effects', 'score', 'text_05A10', 'text_088C0'),
    'm680Dll': ('prolog', 'gameplay', 'character', 'scheduler'),
}


def source_path(module, unit):
    """Units are local stems or explicit shared, decomp-relative C sources.

    A shared entrypoint is compiled in its consuming overlay's namespace,
    never copied or enabled globally just because the source exists.
    """
    return unit if unit.startswith('src/REL/') else f'src/REL/{module}/{unit}.c'


def namespace_flags(decomp, module):
    """Rename only REL-local symbols, identically across all module units.

    Derive anonymous label names from the pinned inputs, not hand-maintained
    lists that silently bind one overlay's declaration to another's definition.
    Native game API names are deliberately outside this pattern.
    """
    root = Path(decomp)
    files = [root / source_path(module, name) for name in MODULES[module]]
    for entry in (root / 'include/REL').iterdir():
        if entry.name.lower().startswith(module[:4].lower()):
            files.extend(entry.rglob('*.h') if entry.is_dir() else [entry])
    return shared_namespace_flags(
        files, module[:4],
        extra_names={'_prolog', '_epilog', '_ctors', '_dtors', 'ObjectSetup'},
    )


def sources(decomp):
    for module, units in MODULES.items():
        # These recovered files include PPC libc's fdlibm.h by its full path.
        # Its matherr/exception ABI is not the host libc ABI. All application
        # consumers use standard math; load those declarations from native libc.
        flags = namespace_flags(decomp, module) + ['-D_FDLIBM_H', '-include', 'math.h']
        if module == 'm668DLL':
            flags += ['-D__fabs=__builtin_fabs', '-include', 'mp6_ppc_math.h']
        if module == 'm629Dll':
            flags += ['-include', 'game/mg/timer.h']
        for name in units:
            unit_flags = list(flags)
            if (module, name) in {('m621dll', 'gameplay'), ('m630dll', 'game')}:
                unit_flags += ['-include', 'game/hsfex.h']
            if (module, name) in {('m621dll', 'gameplay'), ('m619Dll', 'player')}:
                unit_flags += ['-D__fabs=__builtin_fabs']
            if (module, name) == ('m635dll', 'logic'):
                unit_flags += ['-include', 'game/frand.h']
            if (module, name) == ('m633dll', 'middle'):
                unit_flags += ['-include', 'stdlib.h']
            yield source_path(module, name), unit_flags
