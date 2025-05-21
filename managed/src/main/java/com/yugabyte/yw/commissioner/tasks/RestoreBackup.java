package com.yugabyte.yw.commissioner.tasks;

import com.yugabyte.yw.commissioner.BaseTaskDependencies;
import com.yugabyte.yw.commissioner.Common;
import com.yugabyte.yw.commissioner.UserTaskDetails;
import com.yugabyte.yw.commissioner.UserTaskDetails.SubTaskGroupType;
import com.yugabyte.yw.common.backuprestore.ybc.YbcManager;
import com.yugabyte.yw.forms.RestoreBackupParams;
import com.yugabyte.yw.models.Restore;
import com.yugabyte.yw.models.RestoreKeyspace;
import com.yugabyte.yw.models.TaskInfo;
import com.yugabyte.yw.models.Universe;
import java.util.Arrays;
import java.util.concurrent.CancellationException;
import javax.inject.Inject;
import lombok.extern.slf4j.Slf4j;

@Slf4j
public class RestoreBackup extends UniverseTaskBase {

  private YbcManager ybcManager;

  @Inject
  protected RestoreBackup(BaseTaskDependencies baseTaskDependencies, YbcManager ybcManager) {
    super(baseTaskDependencies);
    this.ybcManager = ybcManager;
  }

  @Override
  protected RestoreBackupParams taskParams() {
    return (RestoreBackupParams) taskParams;
  }

  @Override
  public void run() {
    Universe universe = Universe.getOrBadRequest(taskParams().getUniverseUUID());
    Restore restore = null;
    boolean isAbort = false;
    try {
      checkUniverseVersion();
      // Update the universe DB with the update to be performed and set the 'updateInProgress' flag
      // to prevent other updates from happening.
      lockUniverse(-1 /* expectedUniverseVersion */);

      try {

        if (isFirstTry()) {
          backupHelper.validateRestoreOverwrites(taskParams().backupStorageInfoList, universe);
          if (universe.isYbcEnabled()) {
            if (!universe
                .getUniverseDetails()
                .getYbcSoftwareVersion()
                .equals(ybcManager.getStableYbcVersion())) {
              if (universe
                  .getUniverseDetails()
                  .getPrimaryCluster()
                  .userIntent
                  .providerType
                  .equals(Common.CloudType.kubernetes)) {
                createUpgradeYbcTaskOnK8s(
                        taskParams().getUniverseUUID(), ybcManager.getStableYbcVersion())
                    .setSubTaskGroupType(SubTaskGroupType.UpgradingYbc);
              } else {
                createUpgradeYbcTask(
                        taskParams().getUniverseUUID(), ybcManager.getStableYbcVersion(), true)
                    .setSubTaskGroupType(SubTaskGroupType.UpgradingYbc);
              }
            } else {
              // Try re-install ybc if ping check fails
              // Skip upgrade case, since upgrade will anyway re-configure it
              handleUnavailableYbcServers(universe, ybcManager);
            }
          }
        }

        restore =
            createAllRestoreSubtasks(
                taskParams(), UserTaskDetails.SubTaskGroupType.RestoringBackup);

        // Marks the update of this universe as a success only if all the tasks before it succeeded.
        createMarkUniverseUpdateSuccessTasks()
            .setSubTaskGroupType(UserTaskDetails.SubTaskGroupType.ConfigureUniverse);

        // Run all the tasks.
        getRunnableTask().runSubTasks();
        unlockUniverseForUpdate();
        if (restore != null) {
          restore.update(taskUUID, Restore.State.Completed);
        }

      } catch (CancellationException ce) {
        unlockUniverseForUpdate(false);
        isAbort = true;
        // Aborted
        if (restore != null) {
          restore.update(taskUUID, Restore.State.Aborted);
          RestoreKeyspace.update(restore, TaskInfo.State.Aborted);
        }
        throw ce;
      }
    } catch (Throwable t) {
      log.error("Error executing task {} with error='{}'.", getName(), t.getMessage(), t);
      handleFailedBackupAndRestore(
          null, Arrays.asList(restore), isAbort, taskParams().alterLoadBalancer);
      unlockUniverseForUpdate();
      throw t;
    }
    log.info("Finished {} task.", getName());
  }
}
