async page => {
 await page.goto(new URL(page.url()).origin+'/?still');
 await page.waitForFunction(()=>window.proof?.nativeExport);
 const downloadPromise=page.waitForEvent('download');
 await page.evaluate(()=>{
  const data=window.proof.nativeExport();
  data.layout=window.proof.layout;
  const link=document.createElement('a');
  link.href=URL.createObjectURL(new Blob([JSON.stringify(data)],{type:'application/json'}));
  link.download='storybook-native.json';link.click();
  setTimeout(()=>URL.revokeObjectURL(link.href),1000);
 });
 const download=await downloadPromise;
 await download.saveAs('output/playwright/storybook-native.json');
 await page.getByRole('application').screenshot({path:'output/playwright/storybook-integration-reference.png'});
 return {export:'output/playwright/storybook-native.json'};
}
