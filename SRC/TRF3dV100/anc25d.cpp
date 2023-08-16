#include "lupanels.hpp"
#include "anc25d.hpp"

// using LUstruct_v100 as 
/**
Splits a communicator into sub-communicators of size 2^(n+1), where n is incrementing from 0 to log2i(Np)

@param grid3d the grid3d, allowing the number of processors to be known
@return an array of the subcommunicators, with size log2i(Np)
*/
MPI_Comm* anc25d_t::initComm(gridinfo3d_t* grid3d)
{
    int maxLvl = log2i(grid3d->zscp.Np) + 1;
    int myGrid = grid3d->zscp.Iam;
    // crea
    MPI_Comm* zCommOut = (MPI_Comm*) SUPERLU_MALLOC((maxLvl-1)*sizeof(MPI_Comm));
    MPI_Comm zComm = grid3d->zscp.comm;

    // in level-0 communicator is divided as follows:
    // [0,1],[2,3],[4,5],[6,7],[8,9],[10,11],[12,13],[14,15]
    // in level-1 communicator is divided as follows:
    // [0,1,2,3],[4,5,6,7],[8,9,10,11],[12,13,14,15]
    // in level-2 communicator is divided as follows:
    // [0,1,2,3,4,5,6,7],[8,9,10,11,12,13,14,15]
    // in level-3 communicator is divided as follows:
    // [0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15] = zComm 

    for (int_t alvl = 0; alvl < maxLvl-1; ++alvl)
    {
        // find the base communicator

        // lvlCommSize: number of processes in the base communicator
        int lvlCommSize = 1<<(alvl+1);
        // lvlCommBase: rank of the base communicator in original communicator
        // e.g. if in level-0, myGrid = [0,1], then lvlCommBase = 0
        // if in level-1, myGrid = [0,1,2,3], then lvlCommBase = 0 
        //and mygrid in [8,9,10,11] then lvlCommBase = 8
        int lvlCommBase = (myGrid/lvlCommSize)*lvlCommSize;
        // lvlCommRank:MyRank in the base communicator
        int lvlCommRank = myGrid - lvlCommBase;
        MPI_Comm lvlComm;
        MPI_Comm_split(zComm, lvlCommBase,lvlCommRank, &lvlComm);
        zCommOut[alvl] = lvlComm;
        
    }

    return zCommOut;
} 


// A function that prints 

int_t LUstruct_v100::dAncestorFactorBaseline(
    int_t alvl,
    sForest_t *sforest,
    ddiagFactBufs_t **dFBufs, // size maxEtree level
    gEtreeInfo_t *gEtreeInfo, // global etree info
    int tag_ub)
{
    int_t nnodes = sforest->nNodes; // number of nodes in the tree
    if (nnodes < 1)
    {
        return 1;
    }

#if (DEBUGlevel >= 1)
    CHECK_MALLOC(grid3d->iam, "Enter dAncestorFactor_ASYNC()");
#endif

    int_t *perm_c_supno = sforest->nodeList; // list of nodes in the order of factorization
    treeTopoInfo_t *treeTopoInfo = &sforest->topoInfo;
    int_t *myIperm = treeTopoInfo->myIperm;
    int_t maxTopoLevel = treeTopoInfo->numLvl;
    int_t *eTreeTopLims = treeTopoInfo->eTreeTopLims;

    /*main loop over all the levels*/
    int_t numLA = getNumLookAhead(options);

    for (int_t topoLvl = 0; topoLvl < maxTopoLevel; ++topoLvl)
    {
        /* code */
        int_t k_st = eTreeTopLims[topoLvl];
        int_t k_end = eTreeTopLims[topoLvl + 1];
        for (int_t k0 = k_st; k0 < k_end; ++k0)
        {
            int_t k = perm_c_supno[k0];
            int kRoot = anc25d.rootRank(k0, alvl);
            // reduce the l and u panels to the root with MPI_Comm = anc25d.getComm(alvl);
            if (mycol == kcol(k))
            {
                void* sendBuf = (void*) lPanelVec[g2lCol(k)].val;
                if (anc25d.rankHasGrid(k0, alvl))
                    sendBuf = MPI_IN_PLACE;

                MPI_Reduce(sendBuf, lPanelVec[g2lCol(k)].val, 
                           lPanelVec[g2lCol(k)].nzvalSize(), MPI_DOUBLE, MPI_SUM, kRoot, anc25d.getComm(alvl));

            }
                
            if (myrow == krow(k))
            {
                void* sendBuf =  (void*) uPanelVec[g2lRow(k)].val;
                if (anc25d.rankHasGrid(k0, alvl))
                    sendBuf = MPI_IN_PLACE;
                MPI_Reduce(sendBuf, uPanelVec[g2lRow(k)].val, 
                           uPanelVec[g2lRow(k)].nzvalSize(), MPI_DOUBLE, MPI_SUM, kRoot, anc25d.getComm(alvl));
            }
                

            if (anc25d.rankHasGrid(k0, alvl))
            {

                int_t offset = k0 - k_st;
                int_t ksupc = SuperSize(k);
                /*=======   Diagonal Factorization      ======*/
                if (iam == procIJ(k, k))
                {
                    lPanelVec[g2lCol(k)].diagFactor(k, dFBufs[offset]->BlockUFactor, ksupc,
                                                    thresh, xsup, options, stat, info);
                    lPanelVec[g2lCol(k)].packDiagBlock(dFBufs[offset]->BlockLFactor, ksupc);
                }

                /*=======   Diagonal Broadcast          ======*/
                if (myrow == krow(k))
                    MPI_Bcast((void *)dFBufs[offset]->BlockLFactor, ksupc * ksupc,
                              MPI_DOUBLE, kcol(k), (grid->rscp).comm);
                if (mycol == kcol(k))
                    MPI_Bcast((void *)dFBufs[offset]->BlockUFactor, ksupc * ksupc,
                              MPI_DOUBLE, krow(k), (grid->cscp).comm);

                /*=======   Panel Update                ======*/
                if (myrow == krow(k))
                    uPanelVec[g2lRow(k)].panelSolve(ksupc, dFBufs[offset]->BlockLFactor, ksupc);

                if (mycol == kcol(k))
                    lPanelVec[g2lCol(k)].panelSolve(ksupc, dFBufs[offset]->BlockUFactor, ksupc);

                /*=======   Panel Broadcast             ======*/
                upanel_t k_upanel(UidxRecvBufs[0], UvalRecvBufs[0]);
                lpanel_t k_lpanel(LidxRecvBufs[0], LvalRecvBufs[0]);
                if (myrow == krow(k))
                {
                    k_upanel = uPanelVec[g2lRow(k)];
                }
                if (mycol == kcol(k))
                    k_lpanel = lPanelVec[g2lCol(k)];

                if (UidxSendCounts[k] > 0)
                {
                    MPI_Bcast(k_upanel.index, UidxSendCounts[k], mpi_int_t, krow(k), grid3d->cscp.comm);
                    MPI_Bcast(k_upanel.val, UvalSendCounts[k], MPI_DOUBLE, krow(k), grid3d->cscp.comm);
                }

                if (LidxSendCounts[k] > 0)
                {
                    MPI_Bcast(k_lpanel.index, LidxSendCounts[k], mpi_int_t, kcol(k), grid3d->rscp.comm);
                    MPI_Bcast(k_lpanel.val, LvalSendCounts[k], MPI_DOUBLE, kcol(k), grid3d->rscp.comm);
                }

/*=======   Schurcomplement Update      ======*/
#warning single node only
                // dSchurComplementUpdate(k, lPanelVec[g2lCol(k)], uPanelVec[g2lRow(k)]);
                // dSchurComplementUpdate(k, lPanelVec[g2lCol(k)], k_upanel);
                if (UidxSendCounts[k] > 0 && LidxSendCounts[k] > 0)
                {
                    k_upanel.checkCorrectness();
                    dSchurComplementUpdate(k, k_lpanel, k_upanel);
                }
            } /** if (anc25d.rankHasGrid(k0, alvl)) */
            
            // Brodcast the l and u panels to the root with MPI_Comm = anc25d.getComm(alvl);
            if (mycol == kcol(k))
                MPI_Bcast(lPanelVec[g2lCol(k)].val, 
                          lPanelVec[g2lCol(k)].nzvalSize(), MPI_DOUBLE, kRoot, anc25d.getComm(alvl));
                           
            if (myrow == krow(k))
                MPI_Bcast(uPanelVec[g2lRow(k)].val, 
                           uPanelVec[g2lRow(k)].nzvalSize(), MPI_DOUBLE, kRoot, anc25d.getComm(alvl));
            // MPI_Barrier(grid3d->comm);

        } /*for k0= k_st:k_end */

    } /*for topoLvl = 0:maxTopoLevel*/

#if (DEBUGlevel >= 1)
    CHECK_MALLOC(grid3d->iam, "Exit dAncestorFactor_ASYNC()");
#endif

    return 0;
} /* dAncestorFactor_ASYNC */
