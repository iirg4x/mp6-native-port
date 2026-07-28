/* MP6 native port: declarations for the proprietary speech-recognition API.
 *
 * No vendor header survives in the decomp dependency.  The linked native
 * compatibility stubs deliberately use the same K&R `int name()` shape, and
 * every call in game/mic.c exchanges only integer/pointer values.  Declaring
 * that exact surviving ABI prevents host compilers from inventing implicit
 * declarations while keeping the unavailable subsystem fail-closed.
 */
#ifndef MP6_GSAPI_COMPAT_H
#define MP6_GSAPI_COMPAT_H

int gsapi_Close();
int gsapi_ContextActivate();
int gsapi_ContextDeActivate();
int gsapi_ContextSetCtxData();
int gsapi_ContextSetGcdData();
int gsapi_ContextSetParam();
int gsapi_ContextSetWrdData();
int gsapi_EngineClose();
int gsapi_EngineGetParam();
int gsapi_EngineOpen();
int gsapi_EngineRestart();
int gsapi_EngineSessionDataExport();
int gsapi_EngineSessionDataFree();
int gsapi_EngineSessionDataImport();
int gsapi_EngineSetMode();
int gsapi_EngineSetParam();
int gsapi_EngineStart();
int gsapi_EngineStop();
int gsapi_Init();
int gsapi_LanguageLoadBuffer();
int gsapi_LanguageUnLoad();
int gsapi_NotifySetCallback();
int gsapi_SetUserData();

#endif
