#pragma once

class QWindow;

class WindowsWindowManager
{
public:
    static void configureControlWindow(QWindow* window, bool alwaysOnTop);
    static void moveToCurrentDesktop(QWindow* window);
    static bool isOnCurrentDesktop(QWindow* window);
    static void keepAbove(QWindow* window, QWindow* preceding = nullptr,
                          bool preserveOrderBelow = false, bool pinOnAllDesktops = true);
};
