#pragma once

class QWindow;

class WindowsWindowManager
{
public:
    static void keepAboveAndOnAllDesktops(QWindow* window);
};
