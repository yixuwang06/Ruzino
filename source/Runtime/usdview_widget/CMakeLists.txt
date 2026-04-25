if(RUZINO_WITH_OPENUSD)
	set(PRIVATE_LIBS spdlog::spdlog usd usdImaging usdImagingGL hdSt glf garch hgi RHI)
	set(COMPILE_DEFS NOMINMAX=1 RUZINO_BUILD_MODULE=1)

set(PRIVATE_LIBS spdlog::spdlog usd usdImaging usdImagingGL hdSt glf garch hgi RHI)
set(COMPILE_DEFS NOMINMAX=1 RUZINO_BUILD_MODULE=1)

	RUZINO_ADD_LIB(
		usdview_widget
		SHARED
		PUBLIC_LIBS stage geometry GUI
		PRIVATE_LIBS ${PRIVATE_LIBS} hd_RUZINO stage_listener
		COMPILE_DEFS ${COMPILE_DEFS}
	)
	endif()
