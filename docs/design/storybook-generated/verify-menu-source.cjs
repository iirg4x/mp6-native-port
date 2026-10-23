// Source-fidelity check: the design must not invent settings or choice values.
const fs=require('node:fs'),path=require('node:path'),vm=require('node:vm'),assert=require('node:assert/strict');
const root=path.resolve(__dirname,'../../..'),source=fs.readFileSync(path.join(root,'src/gx/ui/settings.cpp'),'utf8');
const context={window:{}};vm.runInNewContext(fs.readFileSync(path.join(__dirname,'chapters.js'),'utf8'),context);
const chapters=JSON.parse(JSON.stringify(context.window.createStoryChapters()));
const expected={Picture:['Preset','Widescreen','Anti-Aliasing','Ambient Occlusion','Shadow Quality','Unlocked FPS','VSync','Show FPS Counter','Window Mode','Window Size','Display'],
 Sound:['Master Volume','More Sound Effects'],Game:['Select Game...','Open Save Folder'],Extras:['Menu Animation','Fast Forward','Free Camera','Extra Memory','About this port']};
const checks=[],check=(condition,label)=>{assert.ok(condition,label);checks.push(label);};
check(chapters.map(c=>c.name).join('|')==='Picture|Sound|Game|Extras','Four chapters only; Controls is excluded by user request');
for(const chapter of chapters){
 const keys=chapter.rows.map(r=>r.sourceKey||r.label);
 assert.deepEqual(keys,expected[chapter.name]);
 const start=source.indexOf('void SettingsWindow::build_'+chapter.name.toLowerCase()+'(');
 const end=source.indexOf('\nvoid SettingsWindow::',start+1),body=source.slice(start,end<0?source.length:end);
 check(keys.every(key=>body.includes('"'+key+'"')),chapter.name+' rows match only existing source entries');
}
const row=name=>chapters.flatMap(c=>c.rows).find(r=>(r.sourceKey||r.label)===name);
check(row('Preset').options.join('|')==='Original|Classic+|Modern','Preset names match kEnhPresetTiers; Custom is derived, not a preset');
check(row('Anti-Aliasing').options.join('|')==='Off|Sharp|Fast|Ultra 1.5x|Ultra 2x','Desktop anti-aliasing choices match source');
check(row('Ambient Occlusion').options.join('|')==='Off|Subtle|Strong','Ambient occlusion choices match source');
check(row('Shadow Quality').options.join('|')==='1x|2x|4x|8x|16x','Shadow quality choices match source');
check(row('Master Volume').options.length===21&&row('Master Volume').options.every((v,i)=>v===(i*5)+'%'),'Master Volume preserves the 0–100 percent, five-point range');
check(chapters[2].rows.every(r=>r.options===null),'Game file/folder actions are not disguised as toggles');
check(row('Free Camera').disabled===true,'Prelaunch Free Camera stays disabled');
check(row('Window Mode').options.join('|')==='Windowed|Fullscreen'&&row('Display').options.join('|')==='Auto|Primary','No fabricated window mode or monitor choices');
console.log(JSON.stringify({passed:checks.length,checks,source:'src/gx/ui/settings.cpp',scope:'desktop prelaunch; conditional drivers/monitors and in-game save slots require runtime context'}));
