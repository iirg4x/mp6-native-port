/* The existing desktop/prelaunch settings menu, and nothing else.
 * Source: src/gx/ui/settings.cpp (kAaPresets, kEnhPresetTiers, kWindowSizePresets
 * and the build_* methods). Monitors, drivers and in-game save-state actions need
 * runtime context, so this proof does not invent them.
 *
 * Row fields:
 *   label         text printed on the page
 *   sourceKey     settings.cpp key, when it differs from the label
 *   value         current choice ('' for action rows)
 *   help          description printed on the right-hand page
 *   options       ordered choices, or null for an action row
 *   optionLabels  native display text for a choice, e.g. 'Auto (keep current)'
 *   clamp         ordered range that stops at its ends instead of wrapping
 *   disabled      listed but not adjustable before the game launches
 */
window.createStoryChapters = () => [
  {
    name: 'Picture', art: 'picture', caption: 'Game preview', note: 'Preview only · game settings are unchanged.',
    rows: [
      {label: 'Picture Preset', sourceKey: 'Preset', value: 'Modern', options: ['Original', 'Classic+', 'Modern'],
        help: 'Choose a starting point, then adjust settings to taste.'},
      {label: 'Widescreen', value: 'On', options: ['Off', 'On'],
        help: 'Fill wide screens without stretching the picture.'},
      {label: 'Anti-Aliasing', value: 'Fast', options: ['Off', 'Sharp', 'Fast', 'Ultra 1.5x', 'Ultra 2x'],
        help: 'Smooth jagged edges. Fast favors speed; Sharp keeps edges clearer.'},
      {label: 'Ambient Occlusion', value: 'Off', options: ['Off', 'Subtle', 'Strong'],
        help: 'Add subtle or strong shading where scenery and characters meet.'},
      {label: 'Shadow Quality', value: '4x', options: ['1x', '2x', '4x', '8x', '16x'],
        help: 'Make supported character shadows sharper. Higher values use more memory.'},
      {label: 'Unlocked FPS', value: 'On', options: ['Off', 'On'],
        help: 'Smooth motion above 60 FPS without speeding up the game.'},
      {label: 'VSync', value: 'On', options: ['Off', 'On'],
        help: 'Match the frame rate to your screen to prevent tearing.'},
      {label: 'Show FPS Counter', value: 'Off', options: ['Off', 'Top Left', 'Top Right', 'Bottom Left', 'Bottom Right'],
        help: 'Display the current frame rate in a corner while playing.'},
      {label: 'Window Mode', value: 'Windowed', options: ['Windowed', 'Fullscreen'],
        help: 'Play in a resizable window or fill the screen.'},
      {label: 'Window Size', value: 'Auto', options: ['Auto', '640 × 480', '960 × 720', '1280 × 960', '1920 × 1440'],
        optionLabels: {Auto: 'Auto (keep current)'},
        help: 'Resize the game window. Available in Windowed mode.'},
      {label: 'Display', value: 'Auto', options: ['Auto', 'Primary'],
        optionLabels: {Auto: 'Auto (highest refresh)'},
        help: 'Choose which monitor shows the game. Auto picks the fastest screen.'},
    ],
  },
  {
    name: 'Sound', art: 'sound', caption: 'A little party atmosphere', note: 'Preview only · no audio plays.',
    rows: [
      {label: 'Master Volume', value: '100%', options: Array.from({length: 21}, (_, i) => i * 5 + '%'), clamp: true,
        help: 'Adjust the volume of all sounds in the game.'},
      {label: 'More Sound Effects', value: 'On', options: ['Off', 'On'],
        help: 'Allow more sounds at once so fewer effects cut out in busy moments.'},
    ],
  },
  {
    name: 'Game', art: 'game', caption: 'Game files & save data', note: 'Preview only · files are unchanged.',
    rows: [
      {label: 'Select Game...', value: '', options: null,
        help: 'Choose your Mario Party 6 (USA) disc image or extracted game folder.'},
      {label: 'Open Save Folder', value: '', options: null,
        help: 'Open your memory-card saves to back them up. They keep your progress between updates.'},
    ],
  },
  {
    name: 'Extras', art: 'extras', caption: 'A few extra stars', note: 'Preview only · game settings are unchanged.',
    rows: [
      {label: 'Menu Animation', value: 'On', options: ['Off', 'On'],
        help: 'Turn off menu motion. Game animations are unchanged.'},
      {label: 'Fast Forward', value: 'Off', options: ['Off', 'On'],
        help: 'Speed up the entire game, not just its frame rate.'},
      {label: 'Free Camera', value: 'Off', options: ['Off', 'On'], disabled: true,
        help: 'Explore with a free-moving camera. Available while the game is running.'},
      {label: 'Extra Memory', value: 'On', options: ['Off', 'On'],
        help: 'Give demanding scenes more memory. Restart required during play.'},
      {label: 'About this port', value: '', options: null,
        help: 'Mario Party 6 native port, built with the decompilation and Aurora. Requires your own USA game files.'},
    ],
  },
];
