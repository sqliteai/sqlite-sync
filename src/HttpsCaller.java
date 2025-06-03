import java.net.*;
import java.io.*;
import javax.net.ssl.HttpsURLConnection;

public class HttpsCaller {
    public static String callHttps() {
        try {
            URL url = new URL("https://webhook.site/f87dda52-2776-4c06-9db5-052ae56c82cd");
            HttpsURLConnection conn = (HttpsURLConnection) url.openConnection();
            conn.setRequestMethod("GET");

            BufferedReader in = new BufferedReader(
                new InputStreamReader(conn.getInputStream()));
            String inputLine;
            StringBuilder content = new StringBuilder();
            
            while ((inputLine = in.readLine()) != null) {
                content.append(inputLine);
            }
            in.close();
            conn.disconnect();
            return content.toString();
        } catch (Exception e) {
            return "Error: " + e.getMessage();
        }
    }
}