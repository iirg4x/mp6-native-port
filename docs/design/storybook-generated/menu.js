/* Menu rules on top of the chapter data: presets, availability and value stepping.
 * Rows are found by label, so reordering a chapter cannot silently retarget a rule.
 */
window.createStoryMenu = chapters => {
  const rowsByLabel = new Map(chapters.flatMap(chapter => chapter.rows).map(row => [row.label, row]));
  const row = label => rowsByLabel.get(label);
  const presetRow = row('Picture Preset');

  // kEnhPresetTiers: each preset sets these rows, in this order. "Custom" is not a
  // preset; it is what the preset row shows when the rows match none of them.
  const presetRows = ['Widescreen', 'Anti-Aliasing', 'Ambient Occlusion', 'Shadow Quality', 'Unlocked FPS',
    'More Sound Effects', 'Extra Memory'];
  const presets = {
    Original: ['Off', 'Off', 'Off', '1x', 'Off', 'Off', 'Off'],
    'Classic+': ['Off', 'Fast', 'Off', '4x', 'On', 'On', 'On'],
    Modern: ['On', 'Fast', 'Off', '4x', 'On', 'On', 'On'],
  };

  function isDisabled(target) {
    return Boolean(target.disabled) || (target.label === 'Window Size' && row('Window Mode').value === 'Fullscreen');
  }

  function displayValue(target) {
    return target.optionLabels?.[target.value] ?? target.value;
  }

  // Moves a row to its next or previous choice. Returns false when nothing changed.
  function step(target, direction) {
    const options = target.options;
    if (!options || isDisabled(target)) return false;
    const current = options.indexOf(target.value);
    let next;
    if (current < 0) next = direction > 0 ? 0 : options.length - 1; // stepping out of "Custom"
    else if (target.clamp) next = Math.min(options.length - 1, Math.max(0, current + direction));
    else next = (current + direction + options.length) % options.length;
    if (next === current) return false;

    target.value = options[next];
    if (target === presetRow) presets[target.value].forEach((value, i) => { row(presetRows[i]).value = value; });
    const values = presetRows.map(label => row(label).value);
    presetRow.value = Object.keys(presets).find(name => presets[name].every((value, i) => value === values[i])) || 'Custom';
    return true;
  }

  return {isDisabled, displayValue, step};
};
