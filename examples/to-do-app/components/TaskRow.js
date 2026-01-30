import { useState, useRef, useEffect } from "react";
import { View, Text, StyleSheet, TouchableOpacity } from "react-native";
import Icon from "@react-native-vector-icons/fontawesome";
import { Swipeable } from "react-native-gesture-handler";

export default TaskRow = ({ task, updateTask, handleDelete }) => {
  const { uuid, title, isCompleted, tag_uuid, tag_name } = task;
  const [checked, setChecked] = useState(isCompleted);
  const swipableRef = useRef(null);

  useEffect(() => {
    setChecked(isCompleted);
  }, [isCompleted]);

  const handleIconPress = () => {
    const newCompletedStatus = checked === 1 ? 0 : 1;
    setChecked(newCompletedStatus);
    updateTask(newCompletedStatus, uuid);
  };

  const renderLeftActions = () => {
    return (
      <TouchableOpacity
        style={styles.deleteButton}
        onPress={() => {
          handleDelete(uuid);
          if (swipableRef.current) {
            swipableRef.current.close();
          }
        }}
      >
        <Text style={styles.deleteButtonText}>Delete</Text>
      </TouchableOpacity>
    );
  };

  return (
    <Swipeable renderLeftActions={renderLeftActions} ref={swipableRef}>
      <View style={styles.taskRow}>
        <View style={styles.taskAndTag}>
          {checked === 0 ? (
            <Text style={styles.text}>{title}</Text>
          ) : (
            <Text style={styles.strikethroughText}>{title}</Text>
          )}
          <Text style={styles.tag}>{tag_name}</Text>
        </View>
        <TouchableOpacity onPress={handleIconPress}>
          <Icon
            name={checked === 1 ? "check-circle" : "circle-thin"}
            size={20}
            color={"#6BA2EA"}
          />
        </TouchableOpacity>
      </View>
      <View style={styles.dottedBox} />
    </Swipeable>
  );
};

const styles = StyleSheet.create({
  taskRow: {
    flexDirection: "row",
    justifyContent: "space-between",
    fontSize: 16,
    padding: 10,
  },
  deleteButton: {
    backgroundColor: "#6BA2EA",
    padding: 10,
    alignItems: "center",
    justifyContent: "center",
  },
  dottedBox: {
    borderWidth: 1,
    borderColor: "lightgray",
    borderStyle: "dashed",
  },
  deleteButtonText: {
    color: "white",
  },
  text: {
    fontSize: 16,
  },
  strikethroughText: {
    textDecorationLine: "line-through",
    textDecorationColor: "#6BA2EA",
    fontSize: 16,
  },
  tag: {
    color: "gray",
    fontSize: 12,
    marginTop: 5,
  },
  taskAndTag: {
    flexDirection: "column",
  },
  actions: {
    flexDirection: "row",
    justifyContent: "center",
    alignItems: "center"
  }
});
