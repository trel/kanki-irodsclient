/**
 * @file rodsdownloadthread.h
 * @brief Implementation of class RodsDownloadThread
 *
 * The RodsDownloadThread class extends the Qt thread management class
 * QThread and implements a worker thread for a download (get) operation.
 *
 * Copyright (C) 2016 KTH Royal Institute of Technology. All rights reserved.
 * License: The BSD 3-Clause License, see LICENSE file for details.
 *
 * Copyright (C) 2014-2016 University of Jyväskylä. All rights reserved.
 * License: The BSD 3-Clause License, see LICENSE file for details.
 *
 * @author Ilari Korhonen
 */

// application class RodsDownloadThread header
#include "rodsdownloadthread.h"

RodsDownloadThread::RodsDownloadThread(Kanki::RodsConnection *theConn, Kanki::RodsObjEntryPtr theObj,
                                       const std::string &theDestPath, bool verifyChecksum, bool allowOverwrite)
    : QThread()
{
    this->conn = new Kanki::RodsConnection(theConn);
    this->objEntry = theObj;
    this->destPath = theDestPath;

    this->verify = verifyChecksum;
    this->overwrite = allowOverwrite;
}

void RodsDownloadThread::run()
{
    int status = 0;
    QString statusStr = "Initializing...";

    irods::thread_pool tank(32);
    auto conn_pool = irods::make_connection_pool(32);

    // signal ui to setup progress display
    progressMarquee(statusStr);

    // open the parallel connection for transfer and authenticate
    if ((status = this->conn->connect()) < 0)
    {
        reportError("Download failed", "Open parallel connection failed", status);
        return;
    }

    else if ((status = this->conn->login()) < 0)
    {
        reportError("Download failed", "Authentication failed", status);
        return;
    }

    // in the case of downloading a collection, do it recursively
    if (this->objEntry->objType == COLL_OBJ_T)
    {
        std::vector<Kanki::RodsObjEntryPtr> collObjs;
        std::string basePath = this->objEntry->collPath;

        // first item to the download object list
        collObjs.push_back(this->objEntry);

        // try to construct the download object list recursively
        if (!(status = makeCollObjList(this->objEntry, &collObjs)))
        {
            // notify ui of progress bar state (object count)
            setupProgressDisplay(statusStr, 0, collObjs.size());

            // iterate thru object list
            for (unsigned int i = 0; i < collObjs.size(); i++)
            {
                Kanki::RodsObjEntryPtr curObj = collObjs.at(i);

                std::string basePath = this->objEntry->getObjectBasePath();
                std::string objPath = curObj->getObjectFullPath();

                objPath.erase(objPath.begin(), objPath.begin() + basePath.size());
                std::string dstPath = this->destPath + objPath;

                // in the case of a data object, we do a get operation
                if (curObj->objType == DATA_OBJ_T)
                {
                    // notify ui of current operation and progress
                    statusStr = "Downloading ";
                    statusStr += curObj->getObjectName().c_str();
                    progressUpdate(statusStr, i+1);

                    // // try to do a rods get operation
		    irods::thread_pool::post(tank, [=, &status] {
			    auto conn = conn_pool->get_connection();			    
			    if ((status = this->downloadFile(conn, curObj, dstPath, this->verify, this->overwrite)) < 0)
				reportError("iRODS get file error", curObj->getObjectFullPath().c_str(), status);
			});

                    // if ((status = this->downloadFile(curObj, dstPath, this->verify, this->overwrite)) < 0)
                    //     reportError("iRODS get file error", curObj->getObjectFullPath().c_str(), status);
                }

                // for collection objects we create the corresponding directory
                else if (curObj->objType == COLL_OBJ_T)
                {
                    // get directory name for ui
                    std::string dirName = dstPath.substr(dstPath.find_last_of('/') + 1);

                    // notify ui
                    statusStr = "Creating directory ";
                    statusStr += dirName.c_str();
                    progressUpdate(statusStr, i+1);

                    // check if directory exists and if not, make it
                    QDir dstDir(dstPath.c_str());

                    if (!dstDir.exists())
                        dstDir.mkpath(dstPath.c_str());
                }
            }
        }
    }

    // in the case of downloading a single data object, a simple get operation
    else if (this->objEntry->objType == DATA_OBJ_T)
    {
        QString statusStr = "Downloading file: ";

        statusStr += this->objEntry->getObjectName().c_str();
        std::string dstPath = this->destPath + "/" + this->objEntry->getObjectName();
        setupProgressDisplay(statusStr, 1, 1);

	auto conn = conn_pool->get_connection();

        // try to do a rods get operation
        if ((status = this->downloadFile(conn, objEntry, dstPath, this->verify, this->overwrite)) < 0)
            reportError("Download failed", "iRODS data stream error", status);
    }

    tank.join();

    this->conn->disconnect();
    delete(this->conn);
}

int RodsDownloadThread::makeCollObjList(Kanki::RodsObjEntryPtr obj, std::vector<Kanki::RodsObjEntryPtr> *objs)
{
    int status = 0;

    // we proceed only for collections
    if (obj->objType == COLL_OBJ_T)
    {
        std::vector<Kanki::RodsObjEntryPtr> curCollObjs;

        // try to read collection
        if ((status = this->conn->readColl(obj->collPath, &curCollObjs)) >= 0)
        {
            // iterate thru current collection
            for (std::vector<Kanki::RodsObjEntryPtr>::iterator i = curCollObjs.begin(); i != curCollObjs.end(); i++)
            {
                // add object to list
                Kanki::RodsObjEntryPtr curObj = *i;
                objs->push_back(curObj);

                // notify ui
                QString statusStr = "Building a list of objects (" + QVariant((int)objs->size()).toString() + ")...";
                progressMarquee(statusStr);

                // recurse on collection objects
                if (curObj->objType == COLL_OBJ_T)
                {
                    status = makeCollObjList(curObj, objs);

                    // on error, back off recursion
                    if (status < 0)
                        return (status);
                }
            }
        }
    }

    return (status);
}

int RodsDownloadThread::downloadFile(irods::connection_pool::connection_proxy &conn,
				     Kanki::RodsObjEntryPtr obj, std::string localPath,
                                     bool verifyChecksum, bool allowOverwrite)
{
    int status = 0;

    // check if we're allowed to proceed
    if (boost::filesystem::exists(localPath) && !allowOverwrite)
        return (OVERWRITE_WITHOUT_FORCE_FLAG);

    // take up namespaces
    namespace io = irods::experimental::io;

    // bring in a transport and a stream
    io::client::default_transport xport(conn);
    io::idstream inStream(xport, obj->getObjectFullPath());

    if (!inStream)
	return (SYS_API_INPUT_ERR);

    std::ofstream outStream(localPath, std::ofstream::binary | 
			    std::ofstream::out);
    
    if (!outStream)
	return (FILE_OPEN_ERR);

    // update status display only on large enough objects
    if (obj->objSize > __KANKI_BUFSIZE_INIT)
        setupSubProgressDisplay("Transferring...", 0, 100);

    // TODO: do parellel transfer if requested
    status = transferFileStream(obj, inStream, outStream);
    
    // close the local stream and rods data stream
    outStream.close();
    inStream.close();

    // TODO: FIXME!
    //inStream.getOprEnd();
    
    // TODO: FIXME!
    // // if we have a successful transfer and if verify checksum was required
    // if (status >= 0 && verifyChecksum && strlen(inStream.checksum()))
    // {
    //     subProgressUpdate("Verifying Checksum...", 100);
    //     status = verifyChksumLocFile((char*)localPath.c_str(), (char*)inStream.checksum(), NULL);
    // }

    return (status);
 }

int RodsDownloadThread::transferFileStream(Kanki::RodsObjEntryPtr obj, std::istream &inStream, std::ofstream &outStream)
{
    long int status = 0, lastRead = 0, totalRead = 0;
    long int readSize = __KANKI_BUFSIZE_MAX;

    char *buffer = (char*)std::malloc(readSize), *buffer2 = (char*)std::malloc(readSize);
    std::thread *writer = nullptr;

    // take namespaces and types
    namespace chrono = std::chrono;
    using clock = chrono::high_resolution_clock;

    using millisec_type = chrono::milliseconds;
    using timepoint_type = clock::time_point;

    // we measure time from the get-go
    timepoint_type t0 = clock::now();

    while (inStream && outStream)
    {
	inStream.read(buffer, __KANKI_BUFSIZE_MAX);
	lastRead = inStream.gcount();

	// time diff after block transfer
        timepoint_type t1 = clock::now();
        millisec_type diff = chrono::duration_cast<millisec_type>(t1 - t0);

        totalRead += lastRead;

	// if we had a thread for writing, wait for it
        if (writer)
        {
            writer->join();

            delete (writer);
	    writer = nullptr;

            // check for write errors
            if (outStream.rdstate () & std::ofstream::badbit)
            {
		status = FILE_WRITE_ERR;
                reportError("Download failed", "Write error", status);

                break;
            }
        }

        // (re)new writer thread and push a lambda function into it
        writer = new std::thread([&outStream, buffer, &lastRead] { 
		outStream.write(buffer, lastRead);
	    }); 
	
        // XOR swap buffer pointers for double buffering
        buffer = (char*)((uintptr_t)buffer ^ (uintptr_t)buffer2);
        buffer2 = (char*)((uintptr_t)buffer ^ (uintptr_t)buffer2);
        buffer = (char*)((uintptr_t)buffer ^ (uintptr_t)buffer2);

        // compute and signal statistics to UI
        double speed = ((double)totalRead / 1048576) / ((double)diff.count() / 1000);
        double percentage = ceil(((double)totalRead / (double)obj->objSize) * 100);

        QString statusStr = "Transferring... " + QVariant((int)percentage).toString() + "%";
        statusStr += " at " + QString::number(speed, 'f', 2) + " MB/s";

        if (obj->objSize > __KANKI_BUFSIZE_INIT)
            subProgressUpdate(statusStr, (int)percentage);
    }

    // free everything we have allocated
    if (writer)
    {
	writer->join();
	delete (writer);
    }

    std::free(buffer);
    std::free(buffer2);

    return (status);
}

int RodsDownloadThread::transferFileParallel(Kanki::RodsObjEntryPtr obj, std::istream &inStream, std::ofstream &outStream)
{
    // TODO: FIXME!
    return (SYS_NOT_SUPPORTED);
}
