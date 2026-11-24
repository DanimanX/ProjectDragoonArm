#include "PDS.h"
#include "debugWindows.h"

s_task* taskListHead;
int numActiveTask;

void PrintDebugTaskHierarchy(s_task* pTask)
{
    ImGui::PushID(0);
    if(ImGui::TreeNode(pTask->m_taskName))
    {
        if (pTask->m4_pSubTask)
        {
            PrintDebugTaskHierarchy(pTask->m4_pSubTask);
        }
        ImGui::TreePop();
    }
    if (pTask->m0_pNextTask)
    {
        PrintDebugTaskHierarchy(pTask->m0_pNextTask);
    }
    ImGui::PopID();
}

void PrintDebugTasksHierarchy()
{
    if (!gDebugWindows.tasks)
        return;
    ImGui::Begin("Tasks", &gDebugWindows.tasks);
    if (taskListHead)
    {
        PrintDebugTaskHierarchy(taskListHead);
    }
    ImGui::End();
}

void PrintDebugTaskInfo(s_task* pTask)
{
#ifndef SHIPPING_BUILD
    pTask->getWorkArea()->Inspect();
    if (pTask->m4_pSubTask)
    {
        PrintDebugTaskInfo(pTask->m4_pSubTask);
    }
    if (pTask->m0_pNextTask)
    {
        PrintDebugTaskInfo(pTask->m0_pNextTask);
    }
#endif
}

void PrintDebugTasksInfo()
{
    if (taskListHead)
    {
        PrintDebugTaskInfo(taskListHead);
    }
}

void processTasks(s_task** ppTask)
{
    s_task* pTask = *ppTask;
    
    do
    {
        ZoneScoped;
        ZoneName(pTask->m_taskName, strlen(pTask->m_taskName));
        if (!(pTask->isFinished()))
        {
            if (pTask->isPaused())
            {
                pTask = *ppTask;

                if (pTask == NULL)
                    return;

                ppTask = &pTask->m0_pNextTask;
                pTask = *ppTask;
                continue;
            }
            if (!pauseEngine[0])
            {
                pTask->getWorkArea()->Update();
            }

            pTask->getWorkArea()->Draw();
        }
        if (pTask->isFinished())
        {
            PDS_CategorizedLog(eLogCategories::log_task, "Task %s is finished\n", pTask->m_taskName);
            pTask->m14_flags ^= TASK_FLAGS_DELETING;
            if (pTask->isDeleting())
            {
                s_task* r4 = pTask->m4_pSubTask;
                while (r4)
                {
                    r4->markFinished();
                    r4 = r4->m0_pNextTask;
                }
            }

            if (pTask->m4_pSubTask)
            {
                processTasks(&pTask->m4_pSubTask);
            }

            // finished but not deleting yet
            if (pTask->isDeleting())
            {
                PDS_CategorizedLog(eLogCategories::log_task, "Task %s is getting deleted\n", pTask->m_taskName);
                pTask->getWorkArea()->Delete();

                numActiveTask--;
            }
            else
            {
                s_heapNode* r4 = pTask->getHeapNode();
                PDS_unimplemented("Disabled freeing task heaps");
                /*
                while (r4)
                {
                    s_heapNode* pNextNode = r4->m_nextNode;
                    freeHeapForTask(pTask->getWorkArea(), r4);
                    r4 = pNextNode;
                }
                */

                *ppTask = pTask->m0_pNextTask;
                delete pTask;

                pTask = *ppTask;
                if (pTask == NULL)
                    return;
                continue;
            }
        }
        else
        {
            if (pTask->m4_pSubTask)
            {
                processTasks(&pTask->m4_pSubTask);
            }
        }

        pTask = *ppTask;

        if (pTask == NULL)
            return;
        
        ppTask = &pTask->m0_pNextTask;
        pTask = *ppTask;
    }while (pTask);
}

// Port diagnostic (PD_TRACE_TASKGRAPH=1): dump the live task hierarchy once a
// second. A stalled scene shows up as a task name that never changes, which is
// the only way to tell "the game is waiting" from "the game is drawing the
// wrong thing" without a debugger on the handheld.
static void pdTraceTaskNode(s_task* t, int depth)
{
    for (; t; t = t->m0_pNextTask)
    {
        fprintf(stderr, "[graph]%*s%s%s\n", depth * 2, "", t->m_taskName ? t->m_taskName : "?",
                t->isFinished() ? " (finished)" : "");
        if (t->m4_pSubTask) pdTraceTaskNode(t->m4_pSubTask, depth + 1);
    }
}

bool pdForceTaskGraphDump = false;

static void pdMaybeTraceTaskGraph()
{
    static const bool on = getenv("PD_TRACE_TASKGRAPH") != nullptr;
    if (!on) return;
    static u32 last = 0;
    const u32 now = SDL_GetTicks();
    if (pdForceTaskGraphDump) { pdForceTaskGraphDump = false; last = now; }
    else if (now - last < 1000) return;
    else last = now;
    fprintf(stderr, "[graph] --- active tasks ---\n");
    pdTraceTaskNode(taskListHead, 0);

    // The numbers that decide whether a scene can advance: the field task's
    // state, the palette fade state, and the menu id. A screen that never
    // changes is usually one of these pinned at the wrong value.
    {
        s_fieldTaskWorkArea* f = getFieldTaskPtr();
        fprintf(stderr, "[state] fieldState=%d status=0x%04x field=%d sub=%d entry=%d fade0=%d fade1=%d 4C=%d 4D=%d menuId=%d tasks=%d\n",
                f ? (int)f->m3C_fieldTaskState : -1,
                f ? (unsigned)f->m28_status : 0u,
                f ? (int)f->m2C_currentFieldIndex : -1,
                f ? (int)f->m2E_currentSubFieldIndex : -1,
                f ? (int)f->m30_fieldEntryPoint : -1,
                (int)g_fadeControls.m0_fade0.m20_stopped,
                (int)g_fadeControls.m24_fade1.m20_stopped,
                (int)g_fadeControls.m_4C, (int)g_fadeControls.m_4D,
                (int)graphicEngineStatus.m40AC.m0_menuId, numActiveTask);
        // Open file handles. The menu graphics task refuses to open its menu
        // until this list is empty, so a leaked handle blacks the screen out
        // forever. Name them.
        int open = 0;
        for (sFileInfoSub* h = fileInfoStruct.m2C_allocatedHead; h; h = h->pNext)
        {
            if (open < 6) fprintf(stderr, "[file] open '%s' size=%u fh=%p\n",
                                  h->m_fileName, h->m_fileSize, (void*)h->fHandle);
            open++;
        }
        if (open) fprintf(stderr, "[file] %d open handle(s)\n", open);
    }
}

void runTasks()
{
    if (taskListHead)
    {
        processTasks(&taskListHead);
    }
    pdMaybeTraceTaskGraph();
}

void resetTasks()
{
    taskListHead = NULL;
    numActiveTask = 0;
}

p_workArea createSubTask(p_workArea parentWorkArea, p_workArea newWorkArea)
{
    s_task* pTask = new s_task;
    assert(pTask);

    numActiveTask++;

    s_task* pParentTask = parentWorkArea->getTask();

    s_task** taskDestination = &pParentTask->m4_pSubTask;
    while (*taskDestination)
    {
        taskDestination = &(*taskDestination)->m0_pNextTask;
    }

    (*taskDestination) = pTask;

    pTask->m0_pNextTask = NULL;
    pTask->m4_pSubTask = NULL;
    pTask->m14_flags = 0;

    newWorkArea->m_pTask = pTask;
    pTask->m_workArea = newWorkArea;

    return pTask->getWorkArea();
}

p_workArea createSubTaskWithArg(p_workArea parentWorkArea, p_workArea newWorkArea)
{
    s_task* pTask = new s_task;
    assert(pTask);

    numActiveTask++;

    s_task* pParentTask = parentWorkArea->getTask();

    s_task** taskDestination = &pParentTask->m4_pSubTask;
    while (*taskDestination)
    {
        taskDestination = &(*taskDestination)->m0_pNextTask;
    }

    (*taskDestination) = pTask;

    pTask->m0_pNextTask = NULL;
    pTask->m4_pSubTask = NULL;
    pTask->m14_flags = 0;

    newWorkArea->m_pTask = pTask;
    pTask->m_workArea = newWorkArea;

    return pTask->getWorkArea();
}

p_workArea createSiblingTaskWithArg(p_workArea workArea, p_workArea pNewWorkArea)
{
    s_task* pTask = new s_task;
    assert(pTask);

    numActiveTask++;

    s_task* pSibling = workArea->getTask();

    s_task** taskDestination = &pSibling->m0_pNextTask;
    while (*taskDestination)
    {
        taskDestination = &(*taskDestination)->m0_pNextTask;
    }

    (*taskDestination) = pTask;

    pTask->m0_pNextTask = NULL;
    pTask->m4_pSubTask = NULL;
    pTask->m14_flags = 0;

    pNewWorkArea->m_pTask = pTask;
    pTask->m_workArea = pNewWorkArea;

    return pTask->getWorkArea();

}

s_workArea* createRootTask(p_workArea newWorkArea)
{
    s_task* pTask = new s_task;
    assert(pTask);

    numActiveTask++;
    taskListHead = pTask;

    pTask->m0_pNextTask = NULL;
    pTask->m4_pSubTask = NULL;
    pTask->m14_flags = 0;

    newWorkArea->m_pTask = pTask;
    pTask->m_workArea = newWorkArea;

    return newWorkArea;
}

s_task* getTaskFromWorkArea(p_workArea pWorkArea)
{
    return (s_task*)(((u8*)pWorkArea) - sizeof(s_task));
}

void dummyTaskInit(s_workArea* pWorkArea)
{
    printf("Unimplmenmented Init task for %s\n", pWorkArea->getTask()->m_taskName);
}

void dummyTaskInitWithArg(s_workArea* pWorkArea, void* arg)
{
    printf("Unimplmenmented Init task for %s\n", pWorkArea->getTask()->m_taskName);
}

void dummyTaskUpdate(s_workArea* pWorkArea)
{
    printf("Unimplmenmented Update task for %s\n", pWorkArea->getTask()->m_taskName);
}

void dummyTaskDraw(s_workArea* pWorkArea)
{
    printf("Unimplmenmented Draw task for %s\n", pWorkArea->getTask()->m_taskName);
}

void dummyTaskDelete(s_workArea* pWorkArea)
{
    printf("Unimplmenmented Delete task for %s\n", pWorkArea->getTask()->m_taskName);
}

