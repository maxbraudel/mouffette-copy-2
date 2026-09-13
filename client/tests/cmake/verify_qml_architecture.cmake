if(NOT DEFINED SOURCE_ROOT)
    message(FATAL_ERROR "SOURCE_ROOT is required")
endif()

file(GLOB_RECURSE APPLICATION_SOURCES
    "${SOURCE_ROOT}/src/*.cpp"
    "${SOURCE_ROOT}/src/*.h"
    "${SOURCE_ROOT}/resources/qml/*.qml")

set(FORBIDDEN_UI_PATTERNS
    "QMainWindow"
    "QQuickWidget"
    "QGraphicsView"
    "QGraphicsScene"
    "QGraphicsEffect"
    "QGraphicsItem"
    "QWidget"
    "QDialog"
    "QFileDialog"
    "QMessageBox"
    "QPushButton"
    "QLabel"
    "QLineEdit"
    "QCheckBox"
    "QComboBox"
    "QListWidget"
    "QTreeWidget"
    "QTableWidget"
    "QMenu"
    "setStyleSheet[ \t]*\\("
    "findChild(ren)?[ \t]*<"
    "ScreenCanvas"
    "LegacyCanvasHost"
    "QuickCanvasRoot"
    "UseQuickCanvasRenderer"
    "useQuickCanvasRenderer"
    "MOUFFETTE_USE_QUICK_CANVAS_RENDERER"
    "use-quick-canvas-renderer")

foreach(SOURCE_FILE IN LISTS APPLICATION_SOURCES)
    file(READ "${SOURCE_FILE}" SOURCE_TEXT)
    foreach(PATTERN IN LISTS FORBIDDEN_UI_PATTERNS)
        if(SOURCE_TEXT MATCHES "${PATTERN}")
            file(RELATIVE_PATH RELATIVE_FILE "${SOURCE_ROOT}" "${SOURCE_FILE}")
            message(FATAL_ERROR
                "Qt Quick architecture gate: forbidden application UI token "
                "'${PATTERN}' in ${RELATIVE_FILE}")
        endif()
    endforeach()
endforeach()

file(READ "${SOURCE_ROOT}/CMakeLists.txt" BUILD_TEXT)
foreach(FORBIDDEN_MODULE IN ITEMS QuickWidgets MultimediaWidgets SvgWidgets)
    if(BUILD_TEXT MATCHES "Qt6::${FORBIDDEN_MODULE}|COMPONENTS[^\n]*${FORBIDDEN_MODULE}")
        message(FATAL_ERROR
            "Qt Quick architecture gate: forbidden Qt module ${FORBIDDEN_MODULE}")
    endif()
endforeach()

file(GLOB_RECURSE QSS_FILES
    "${SOURCE_ROOT}/src/*.qss"
    "${SOURCE_ROOT}/resources/*.qss")
if(QSS_FILES)
    message(FATAL_ERROR "Qt Quick architecture gate: QSS files remain: ${QSS_FILES}")
endif()

message(STATUS "Qt Quick architecture gate passed")
