#pragma once

#if defined(__EMSCRIPTEN__)
#include <emscripten/emscripten.h>
#endif

static inline void WEB_NavigateToGamePageFromQueryParamG()
{
#if defined(__EMSCRIPTEN__)
	EM_ASM({
		try {
			const u = new URL(window.location.href);
			const gameId = u.searchParams.get('g');
			if (gameId) {
				window.location.assign(`/servers/${encodeURIComponent(String(gameId))}`);
			} else {
				window.location.assign('/servers');
			}
		} catch (e) {
			console.error('quit redirect failed:', e);
			try { window.location.assign('/servers'); } catch (_) {}
		}
	});
#endif
}

static inline void WEB_NavigateToGamePageFromQueryParamG_Delayed(const int delayMs)
{
#if defined(__EMSCRIPTEN__)
	EM_ASM({
		const ms = $0|0;
		setTimeout(() => {
			try {
				const u = new URL(window.location.href);
				const gameId = u.searchParams.get('g');
				if (gameId) {
					window.location.assign(`/servers/${encodeURIComponent(String(gameId))}`);
				} else {
					window.location.assign('/servers');
				}
			} catch (e) {
				console.error('quit redirect failed:', e);
				try { window.location.assign('/servers'); } catch (_) {}
			}
		}, Math.max(0, ms));
	}, delayMs);
#else
	(void)delayMs;
	WEB_NavigateToGamePageFromQueryParamG();
#endif
}
