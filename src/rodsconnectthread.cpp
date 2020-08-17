/**
 * @file rodsconnectthread.cpp
 * @brief Implementation of class RodsConnectThread
 *
 * The RodsConnectThread class extends the Qt thread management class
 * QThread and implements a worker thread for establishing a
 * connection to an iRODS server.
 *
 * Copyright (C) 2016 KTH Royal Institute of Technology. All rights reserved.
 * License: The BSD 3-Clause License, see LICENSE file for details.
 *
 * Copyright (C) 2014-2016 University of Jyväskylä. All rights reserved.
 * License: The BSD 3-Clause License, see LICENSE file for details.
 *
 * @author Ilari Korhonen
 */

// application class RodsConnectThread header
#include "rodsconnectthread.h"

void RodsConnectThread::run()
{
    Kanki::RodsSession *newSession = nullptr;
    int status = 0;

    progressUpdate("Connecting to iRODS...", 1);
    newSession = new Kanki::RodsSession();

    // try to connect to the iRODS server
    if ((status = newSession->connect()) < 0)
    {
        reportError("iRODS connection error", newSession->lastErrorMsg().c_str(),
                    newSession->lastError());

        delete(newSession);

        // signal connection attempt failure
        setConnection(nullptr);
        failure();
    }

    // second phase - user authentication
    else {
        progressUpdate("Authenticating...", 2);

        // try to authenticate while reporting error trough ui
        if ((status = newSession->login()) < 0)
        {
            delete(newSession);

            setConnection(nullptr);
            authFailure();
        }

        // on success, signal out the newly created connection object
        else {
            setConnection(newSession);
            success();
        }
    }
}
