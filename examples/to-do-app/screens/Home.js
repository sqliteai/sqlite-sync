import { useState } from "react";
import { View, Text, StyleSheet, FlatList, Alert } from "react-native";
import { Button } from "react-native-paper";
import Icon from "@react-native-vector-icons/fontawesome";
import TaskRow from "../components/TaskRow";
import AddTaskModal from "../components/AddTaskModal";
import useTasks from "../hooks/useTasks"

export default Home = ({ route }) => {
  const [modalVisible, setModalVisible] = useState(false);

  const tag = route.params?.category;

  const { taskList, updateTask, addTaskTag, deleteTask } = useTasks(tag)

  const today = new Date();
  const options = { year: "numeric", month: "long", day: "numeric" };
  const formattedDate = today.toLocaleDateString("en-US", options);

  const handleDelete = (taskUuid) => {
    Alert.alert(
      "Confirm Delete",
      "Are you sure you want to delete this task?",
      [
        {
          text: "Cancel",
          style: "cancel",
        },
        {
          text: "Delete",
          onPress: () => deleteTask(taskUuid),
          style: "destructive",
        },
      ]
    );
  };

  return (
    <View style={styles.container}>
      <Text style={styles.date}>{formattedDate}</Text>
      <FlatList
        style={styles.taskList}
        data={taskList}
        keyExtractor={(item, index) => index}
        renderItem={({ item }) => (
          <TaskRow
            task={item}
            updateTask={updateTask}
            handleDelete={handleDelete}
          />
        )}
      />
      <Button
        style={styles.button}
        onPress={() => {
          setModalVisible(true);
        }}
      >
        <Icon name="plus" color={"white"} />
      </Button>
      <AddTaskModal
        modalVisible={modalVisible}
        addTaskTag={addTaskTag}
        setModalVisible={setModalVisible}
      />
    </View>
  );
};

const styles = StyleSheet.create({
  container: {
    flex: 1,
    backgroundColor: "white",
    padding: 20,
  },
  date: {
    color: "gray",
    marginTop: 50,
    fontSize: 16,
  },
  button: {
    backgroundColor: "#6BA2EA",
    position: "absolute",
    bottom: 70,
    right: 20,
  },
  taskList: {
    paddingTop: 40,
  },
});
