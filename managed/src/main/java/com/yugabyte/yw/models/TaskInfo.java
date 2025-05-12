// Copyright (c) YugaByte, Inc.

package com.yugabyte.yw.models;

import static com.yugabyte.yw.commissioner.UserTaskDetails.createSubTask;
import static com.yugabyte.yw.models.helpers.CommonUtils.appendInClause;
import static io.swagger.annotations.ApiModelProperty.AccessMode.READ_ONLY;
import static play.mvc.Http.Status.BAD_REQUEST;

import com.fasterxml.jackson.annotation.JsonIgnore;
import com.fasterxml.jackson.annotation.JsonProperty;
import com.fasterxml.jackson.databind.JsonNode;
import com.google.common.collect.Sets;
import com.yugabyte.yw.commissioner.TaskExecutor.TaskCache;
import com.yugabyte.yw.commissioner.UserTaskDetails;
import com.yugabyte.yw.commissioner.UserTaskDetails.SubTaskDetails;
import com.yugabyte.yw.commissioner.UserTaskDetails.SubTaskGroupType;
import com.yugabyte.yw.common.PlatformServiceException;
import com.yugabyte.yw.common.RedactingService;
import com.yugabyte.yw.common.concurrent.KeyLock;
import com.yugabyte.yw.models.helpers.TaskDetails;
import com.yugabyte.yw.models.helpers.TaskType;
import com.yugabyte.yw.models.helpers.TransactionUtil;
import com.yugabyte.yw.models.helpers.YBAError;
import io.ebean.ExpressionList;
import io.ebean.FetchGroup;
import io.ebean.Finder;
import io.ebean.Model;
import io.ebean.annotation.DbJson;
import io.ebean.annotation.EnumValue;
import io.ebean.annotation.WhenCreated;
import io.ebean.annotation.WhenModified;
import io.swagger.annotations.ApiModel;
import io.swagger.annotations.ApiModelProperty;
import jakarta.persistence.Column;
import jakarta.persistence.Entity;
import jakarta.persistence.EnumType;
import jakarta.persistence.Enumerated;
import jakarta.persistence.Id;
import java.util.Collection;
import java.util.Collections;
import java.util.Date;
import java.util.HashMap;
import java.util.HashSet;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.Set;
import java.util.UUID;
import java.util.concurrent.atomic.AtomicReference;
import java.util.function.Consumer;
import lombok.AccessLevel;
import lombok.Getter;
import lombok.Setter;
import org.apache.commons.collections4.CollectionUtils;
import play.data.validation.Constraints;

@Entity
@ApiModel(description = "Task information")
@Getter
@Setter
public class TaskInfo extends Model {

  private static final FetchGroup<TaskInfo> GET_SUBTASKS_FG =
      FetchGroup.of(TaskInfo.class, "uuid, subTaskGroupType, taskState");

  // This is a key lock for task info by UUID.
  private static final KeyLock<UUID> TASK_INFO_KEY_LOCK = new KeyLock<UUID>();

  public static final Set<State> COMPLETED_STATES =
      Sets.immutableEnumSet(State.Success, State.Failure, State.Aborted);

  public static final Set<State> ERROR_STATES = Sets.immutableEnumSet(State.Failure, State.Aborted);

  public static final Set<State> INCOMPLETE_STATES =
      Sets.immutableEnumSet(State.Created, State.Initializing, State.Running, State.Abort);

  /** These are the various states of the task and taskgroup. */
  public enum State {
    @EnumValue("Created")
    Created(3),

    @EnumValue("Initializing")
    Initializing(1),

    @EnumValue("Running")
    Running(4),

    @EnumValue("Success")
    Success(2),

    @EnumValue("Failure")
    Failure(7),

    @EnumValue("Unknown")
    Unknown(0),

    @EnumValue("Abort")
    Abort(5),

    @EnumValue("Aborted")
    Aborted(6);

    // State override precedence to report the aggregated state for a SubGroupType.
    private final int precedence;

    private State(int precedence) {
      this.precedence = precedence;
    }

    public int getPrecedence() {
      return precedence;
    }
  }

  // The task UUID.
  @Id
  @ApiModelProperty(value = "Task UUID", accessMode = READ_ONLY)
  private UUID uuid;

  // The UUID of the parent task (if any; CustomerTasks have no parent)
  @ApiModelProperty(value = "Parent task UUID", accessMode = READ_ONLY)
  private UUID parentUuid;

  // The position within the parent task's taskQueue (-1 for a CustomerTask)
  @Column(columnDefinition = "integer default -1")
  @ApiModelProperty(
      value = "The task's position with its parent task's queue",
      accessMode = READ_ONLY)
  private Integer position = -1;

  // The task type.
  @Column(nullable = false)
  @Enumerated(EnumType.STRING)
  @ApiModelProperty(value = "Task type", accessMode = READ_ONLY)
  private final TaskType taskType;

  // The task state.
  @Column(nullable = false)
  @Enumerated(EnumType.STRING)
  @ApiModelProperty(value = "Task state", accessMode = READ_ONLY)
  private State taskState = State.Created;

  // The subtask group type (if it is a subtask)
  @Enumerated(EnumType.STRING)
  @ApiModelProperty(value = "Subtask type", accessMode = READ_ONLY)
  private UserTaskDetails.SubTaskGroupType subTaskGroupType;

  // The task creation time.
  @WhenCreated
  @ApiModelProperty(value = "Creation time", accessMode = READ_ONLY, example = "1624295239113")
  private Date createTime;

  // The task update time. Time of the latest update (including heartbeat updates) on this task.
  @WhenModified
  @ApiModelProperty(value = "Updated time", accessMode = READ_ONLY, example = "1624295239113")
  private Date updateTime;

  // The percentage completeness of the task, which is a number from 0 to 100.
  @Column(columnDefinition = "integer default 0")
  @ApiModelProperty(value = "Percentage complete", accessMode = READ_ONLY)
  private Integer percentDone = 0;

  // Task input parameters.
  @Constraints.Required
  @Column(columnDefinition = "TEXT default '{}'", nullable = false)
  @DbJson
  @ApiModelProperty(value = "Task params", accessMode = READ_ONLY, required = true)
  private JsonNode taskParams;

  // Execution or runtime details of the task.
  @Setter(AccessLevel.NONE)
  @Constraints.Required
  @Column(columnDefinition = "TEXT")
  @DbJson
  @ApiModelProperty(value = "Task details", accessMode = READ_ONLY)
  private TaskDetails details;

  // Identifier of the process owning the task.
  @Constraints.Required
  @Column(nullable = false)
  @ApiModelProperty(
      value = "ID of the process that owns this task",
      accessMode = READ_ONLY,
      required = true)
  private String owner;

  public TaskInfo(TaskType taskType, UUID taskUUID) {
    this.taskType = taskType;
    this.uuid = taskUUID;
  }

  /**
   * Update the task info record in transaction. Use this for updates by tasks.
   *
   * @param taskUuid the task UUID.
   * @param updater the updater to change the fields.
   * @return the updated task info.
   */
  public static TaskInfo updateInTxn(UUID taskUuid, Consumer<TaskInfo> updater) {
    TASK_INFO_KEY_LOCK.acquireLock(taskUuid);
    try {
      // Perform the below code block in transaction.
      AtomicReference<TaskInfo> taskInfoRef = new AtomicReference<>();
      TransactionUtil.doInTxn(
          () -> {
            TaskInfo taskInfo = TaskInfo.getOrBadRequest(taskUuid);
            updater.accept(taskInfo);
            taskInfo.save();
          },
          TransactionUtil.DEFAULT_RETRY_CONFIG);
      return taskInfoRef.get();
    } finally {
      TASK_INFO_KEY_LOCK.releaseLock(taskUuid);
    }
  }

  /**
   * Inherit properties or fields from the previous task info on retry.
   *
   * @param previousTaskInfo the previous task info.
   */
  public void inherit(TaskInfo previousTaskInfo) {
    setRuntimeInfo(previousTaskInfo.getRuntimeInfo());
  }

  @JsonIgnore
  public String getErrorMessage() {
    YBAError error = getTaskError();
    if (error != null) {
      return error.getMessage();
    }
    return null;
  }

  @JsonIgnore
  public synchronized YBAError getTaskError() {
    if (taskState == State.Success || details == null) {
      return null;
    }
    YBAError error = details.getError();
    if (error == null || error.getCode() == null) {
      return null;
    }
    return error;
  }

  @JsonIgnore
  public synchronized void setTaskError(YBAError error) {
    if (details == null) {
      details = new TaskDetails();
    }
    details.setError(error);
  }

  @JsonIgnore
  public synchronized String getVersion() {
    return details == null ? "" : details.getVersion();
  }

  @JsonIgnore
  public synchronized void setVersion(String version) {
    if (details == null) {
      details = new TaskDetails();
    }
    details.setVersion(version);
  }

  @JsonIgnore
  public synchronized JsonNode getRuntimeInfo() {
    if (details == null) {
      return null;
    }
    return details.getRuntimeInfo();
  }

  @JsonIgnore
  public synchronized void setRuntimeInfo(JsonNode taskRuntimeInfo) {
    if (details == null) {
      details = new TaskDetails();
    }
    details.setRuntimeInfo(taskRuntimeInfo);
  }

  public boolean hasCompleted() {
    return COMPLETED_STATES.contains(taskState);
  }

  public UUID getTaskUUID() {
    return uuid;
  }

  public void setTaskUUID(UUID taskUUID) {
    uuid = taskUUID;
  }

  @JsonIgnore
  public JsonNode getTaskParams() {
    return taskParams;
  }

  @JsonProperty("taskParams")
  public JsonNode getRedactedParams() {
    return RedactingService.filterSecretFields(taskParams, RedactingService.RedactionTarget.LOGS);
  }

  public static final Finder<UUID, TaskInfo> find = new Finder<UUID, TaskInfo>(TaskInfo.class) {};

  @Deprecated
  public static TaskInfo get(UUID taskUUID) {
    // Return the instance details object.
    return find.byId(taskUUID);
  }

  public static Optional<TaskInfo> maybeGet(UUID taskUUID) {
    return Optional.ofNullable(get(taskUUID));
  }

  public static TaskInfo getOrBadRequest(UUID taskUUID) {
    TaskInfo taskInfo = get(taskUUID);
    if (taskInfo == null) {
      throw new PlatformServiceException(BAD_REQUEST, "Invalid task info UUID: " + taskUUID);
    }
    return taskInfo;
  }

  public static List<TaskInfo> find(Collection<UUID> taskUUIDs) {
    // Return the instance details object.
    if (CollectionUtils.isEmpty(taskUUIDs)) {
      return Collections.emptyList();
    }
    Set<UUID> uniqueTaskUUIDs = new HashSet<>(taskUUIDs);
    ExpressionList<TaskInfo> query = find.query().where();
    appendInClause(query, "uuid", uniqueTaskUUIDs);
    return query.findList();
  }

  public static List<TaskInfo> getLatestIncompleteBackupTask() {
    return find.query()
        .where()
        .eq("task_type", TaskType.CreateBackup)
        .notIn("task_state", COMPLETED_STATES)
        .orderBy("create_time desc")
        .findList();
  }

  // Returns  partial object
  @JsonIgnore
  public List<TaskInfo> getSubTasks() {
    ExpressionList<TaskInfo> subTaskQuery =
        TaskInfo.find
            .query()
            .select(GET_SUBTASKS_FG)
            .where()
            .eq("parent_uuid", getTaskUUID())
            .orderBy("position asc");
    return subTaskQuery.findList();
  }

  @JsonIgnore
  public List<TaskInfo> getIncompleteSubTasks() {
    return TaskInfo.find
        .query()
        .select(GET_SUBTASKS_FG)
        .where()
        .eq("parent_uuid", getTaskUUID())
        .in("task_state", INCOMPLETE_STATES)
        .findList();
  }

  @Override
  public String toString() {
    StringBuilder sb = new StringBuilder();
    sb.append("taskType : ").append(taskType);
    sb.append(", ");
    sb.append("taskState: ").append(taskState);
    return sb.toString();
  }

  @JsonIgnore
  public UserTaskDetails getUserTaskDetails() {
    return getUserTaskDetails(null);
  }

  /**
   * Retrieve the UserTaskDetails for the task mapped to this TaskInfo object. Should only be called
   * on the user-level parent task, since only that task will have subtasks. Nothing will break if
   * called on a SubTask, it just won't give you much useful information. Some subtask group types
   * are repeated later in the task that must be fixed. So, a subTask group cannot be marked
   * Success.
   *
   * @return UserTaskDetails object for this TaskInfo, including info on the state on each of the
   *     subTaskGroups.
   */
  public UserTaskDetails getUserTaskDetails(TaskCache taskCache) {
    UserTaskDetails taskDetails = new UserTaskDetails();
    List<TaskInfo> result = getSubTasks();
    Map<SubTaskGroupType, SubTaskDetails> userTasksMap = new HashMap<>();
    SubTaskGroupType lastGroupType = SubTaskGroupType.Configuring;
    for (TaskInfo taskInfo : result) {
      SubTaskGroupType subTaskGroupType = taskInfo.getSubTaskGroupType();
      SubTaskDetails subTask = null;
      if (userTasksMap.containsKey(subTaskGroupType)) {
        // The type is already seen, group it with the last task if it is present.
        // This is done not to move back the progress for the group type on the UI
        // if the type shows up later.
        subTask = userTasksMap.get(lastGroupType);
      }
      if (subTask == null) {
        subTask = createSubTask(subTaskGroupType);
        taskDetails.add(subTask);
        userTasksMap.put(subTaskGroupType, subTask);
        // Move only when it is new.
        // This works for patterns like A B A B C.
        lastGroupType = subTaskGroupType;
      }
      if (taskCache != null) {
        // Populate extra details about task progress from Task Cache.
        JsonNode cacheData = taskCache.get(taskInfo.getTaskUUID().toString());
        subTask.populateDetails(cacheData);
      }
      if (subTask.getState().getPrecedence() < taskInfo.getTaskState().getPrecedence()) {
        State overrideState = taskInfo.getTaskState();
        if (subTask.getState() == State.Success && taskInfo.getTaskState() == State.Created) {
          // SubTask was already running, so skip to running to fix this very short transition.
          overrideState = State.Running;
        }
        subTask.setState(overrideState);
      }
    }
    return taskDetails;
  }

  /**
   * Returns the aggregate percentage completion across all the subtasks.
   *
   * @return a number between 0.0 and 100.0.
   */
  @JsonIgnore
  public double getPercentCompleted() {
    if (getTaskState() == TaskInfo.State.Success) {
      return 100.0;
    }
    int numSubtasks = TaskInfo.find.query().where().eq("parent_uuid", getUuid()).findCount();
    if (numSubtasks == 0) {
      return 0.0;
    }
    int numSubtasksCompleted =
        TaskInfo.find
            .query()
            .where()
            .eq("parent_uuid", getTaskUUID())
            .eq("task_state", TaskInfo.State.Success)
            .findCount();
    return numSubtasksCompleted * 100.0 / numSubtasks;
  }

  public static List<TaskInfo> findDuplicateDeleteBackupTasks(UUID customerUUID, UUID backupUUID) {
    return TaskInfo.find
        .query()
        .where()
        .eq("task_type", TaskType.DeleteBackup)
        .ne("task_state", State.Failure)
        .ne("task_state", State.Aborted)
        .eq("task_params->>'customerUUID'", customerUUID.toString())
        .eq("task_params->>'backupUUID'", backupUUID.toString())
        .findList();
  }

  public static List<TaskInfo> findIncompleteDeleteBackupTasks(UUID customerUUID, UUID backupUUID) {
    return TaskInfo.find
        .query()
        .where()
        .in("task_type", TaskType.DeleteBackup, TaskType.DeleteBackupYb)
        .in("task_state", INCOMPLETE_STATES)
        .eq("task_params->>'customerUUID'", customerUUID.toString())
        .eq("task_params->>'backupUUID'", backupUUID.toString())
        .findList();
  }
}
