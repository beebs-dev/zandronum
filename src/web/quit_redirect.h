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
				window.location.assign(`/game/${encodeURIComponent(String(gameId))}`);
			} else {
				window.location.assign('/game');
			}
		} catch (e) {
			console.error('quit redirect failed:', e);
			try { window.location.assign('/game'); } catch (_) {}
		}
	});
#endif
}
