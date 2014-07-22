/** @file
 * VBox Qt GUI - UIMachineLogicScale class declaration.
 */

/*
 * Copyright (C) 2010-2013 Oracle Corporation
 *
 * This file is part of VirtualBox Open Source Edition (OSE), as
 * available from http://www.virtualbox.org. This file is free software;
 * you can redistribute it and/or modify it under the terms of the GNU
 * General Public License (GPL) as published by the Free Software
 * Foundation, in version 2 as it comes in the "COPYING" file of the
 * VirtualBox OSE distribution. VirtualBox OSE is distributed in the
 * hope that it will be useful, but WITHOUT ANY WARRANTY of any kind.
 */

#ifndef ___UIMachineLogicScale_h___
#define ___UIMachineLogicScale_h___

/* Local includes: */
#include "UIMachineLogic.h"

/* Scale machine logic implementation: */
class UIMachineLogicScale : public UIMachineLogic
{
    Q_OBJECT;

protected:

    /* Constructor: */
    UIMachineLogicScale(QObject *pParent, UISession *pSession);

    /* Check if this logic is available: */
    bool checkAvailability();

#ifndef RT_OS_DARWIN
private slots:

    /** Invokes popup-menu. */
    void sltInvokePopupMenu();
#endif /* !RT_OS_DARWIN */

private:

    /* Prepare helpers: */
    void prepareActionGroups();
    void prepareActionConnections();
    void prepareMachineWindows();
#ifndef RT_OS_DARWIN
    void prepareMenu();
#endif /* !RT_OS_DARWIN */

    /* Cleanup helpers: */
#ifndef RT_OS_DARWIN
    void cleanupMenu();
#endif /* !RT_OS_DARWIN */
    void cleanupMachineWindows();
    void cleanupActionConnections();
    void cleanupActionGroups();

#ifndef RT_OS_DARWIN
    /** Holds the popup-menu instance. */
    QMenu *m_pPopupMenu;
#endif /* !RT_OS_DARWIN */

    /* Friend classes: */
    friend class UIMachineLogic;
};

#endif /* !___UIMachineLogicScale_h___ */

