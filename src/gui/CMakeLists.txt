find_package(Qt6 6.5 REQUIRED COMPONENTS Widgets)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

add_executable(Decomp2Gecko-gui
	main.cpp
	MainWindow.cpp
	MainWindow.h
	ReportPane.cpp
	ReportPane.h
	JobRunner.cpp
	JobRunner.h
	InstallDialog.cpp
	InstallDialog.h
	${CMAKE_SOURCE_DIR}/packaging/Decomp2Gecko.qrc
)
target_link_libraries(Decomp2Gecko-gui PRIVATE Decomp2Gecko_core Qt6::Widgets Decomp2Gecko_warnings)

if(APPLE)
	set_target_properties(Decomp2Gecko-gui PROPERTIES
		MACOSX_BUNDLE TRUE
		MACOSX_BUNDLE_BUNDLE_NAME "Decomp2Gecko"
		MACOSX_BUNDLE_GUI_IDENTIFIER "io.github.johnqherman.Decomp2Gecko"
		MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
		MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION}
		MACOSX_BUNDLE_ICON_FILE Decomp2Gecko.icns
	)
	target_sources(Decomp2Gecko-gui PRIVATE ${CMAKE_SOURCE_DIR}/packaging/Decomp2Gecko.icns)
	set_source_files_properties(${CMAKE_SOURCE_DIR}/packaging/Decomp2Gecko.icns PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
	install(TARGETS Decomp2Gecko-gui BUNDLE DESTINATION .)
elseif(WIN32)
	set_target_properties(Decomp2Gecko-gui PROPERTIES WIN32_EXECUTABLE TRUE)
	enable_language(RC)
	target_sources(Decomp2Gecko-gui PRIVATE ${CMAKE_SOURCE_DIR}/packaging/Decomp2Gecko.rc)
	target_include_directories(Decomp2Gecko-gui PRIVATE ${CMAKE_SOURCE_DIR}/packaging)
	install(TARGETS Decomp2Gecko-gui RUNTIME DESTINATION .)
else()
	include(GNUInstallDirs)
	install(TARGETS Decomp2Gecko-gui RUNTIME DESTINATION bin)
	install(FILES ${CMAKE_SOURCE_DIR}/packaging/Decomp2Gecko.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
	install(FILES ${CMAKE_SOURCE_DIR}/packaging/Decomp2Gecko.svg DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)
	install(DIRECTORY ${CMAKE_SOURCE_DIR}/packaging/hicolor DESTINATION ${CMAKE_INSTALL_DATADIR}/icons)
endif()

option(DECOMP2GECKO_DEPLOY_QT "Bundle Qt runtime dependencies on install" OFF)
if(DECOMP2GECKO_DEPLOY_QT)
	qt_generate_deploy_app_script(
		TARGET Decomp2Gecko-gui
		OUTPUT_SCRIPT deploy_script
	)
	install(SCRIPT ${deploy_script})
endif()
