/*
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The ASF licenses this file to You under the Apache License, Version 2.0
 * (the "License"); you may not use this file except in compliance with
 * the License.  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

using System;
using System.Collections.Generic;
using System.IO;
using System.Runtime.Remoting;
using System.Runtime.Remoting.Channels;
using System.Runtime.Remoting.Channels.Tcp;
using System.Runtime.Serialization.Formatters;

namespace Apache.Geode.Client.FwkLauncher
{
  using Apache.Geode.DUnitFramework;
  class LauncherProcess
  {
    public static IChannel clientChannel = null;
    public static string logFile = null;
    // This port has been fixed for FwkLauncher.
    static void Main(string[] args)
    {
      string myId = "0";
      try
      {
        int launcherPort = 0;
        string driverUrl;
        ParseArguments(args, out driverUrl, out myId, out logFile, out launcherPort);
        // NOTE: This is required so that remote client receive custom exceptions
        RemotingConfiguration.CustomErrorsMode = CustomErrorsModes.Off;

        BinaryServerFormatterSinkProvider serverProvider =
          new BinaryServerFormatterSinkProvider();
        serverProvider.TypeFilterLevel = TypeFilterLevel.Full;
        BinaryClientFormatterSinkProvider clientProvider =
          new BinaryClientFormatterSinkProvider();
        Dictionary<string, string> properties;

        #region Create the communication channel to receive commands from server

        properties = new Dictionary<string, string>();
        properties["port"] = launcherPort.ToString();
        clientChannel = new TcpChannel(properties, clientProvider, serverProvider);
        
        ChannelServices.RegisterChannel(clientChannel, false);

        RemotingConfiguration.RegisterWellKnownServiceType(typeof(LauncherComm),
          CommConstants.ClientService, WellKnownObjectMode.SingleCall);

        #endregion

        Util.ClientId = myId;
        Util.LogFile = logFile;
        if (!string.IsNullOrEmpty(driverUrl))
        {
          Util.DriverComm = ServerConnection<IDriverComm>.Connect(driverUrl);
          Util.ClientListening();
        }
      }
      catch (Exception ex)
      {
        Util.Log("FATAL: Client {0}, Exception caught: {1}", myId, ex);
      }
      System.Threading.Thread.Sleep(System.Threading.Timeout.Infinite);
    }

    private static void ShowUsage(string[] args)
    {
      if (args != null)
      {
        Util.Log("Args: ");
        foreach (string arg in args)
        {
          Util.Log("\t{0}", arg);
        }
      }
      string procName = Util.ProcessName;
      Util.Log("Usage: " + procName + " [OPTION]");
      Util.Log("Options are:");
      Util.Log("  --id=ID \t\t ID of the launcher; process ID is used when not provided");
      Util.Log("  --port=PORT \t\t Port number where the launcher listens for incoming requests.");
      Util.Log("  --driver=URL \t Optional. The URL (e.g. tcp://<host>:<port>/<service>) of the Driver.");
      Util.Log("  --log=LOGFILE \t Optional. The name of the logfile; standard output is used when not provided");
      Util.Log("  --startdir=DIR \t Optional. Start in the given directory");
      Util.Log("  --bg \t Optional. Start in background");
      Environment.Exit(1);
    }

    private static void ParseArguments(string[] args, out string driverUrl, out string myId,
      out string logFile, out int launcherPort)
    {
      if (args == null)
      {
        ShowUsage(args);
      }
      string IDOption = "--id=";
      string DriverOption = "--driver=";
      string LogOption = "--log=";
      string StartDirOption = "--startdir=";
      string BGOption = "--bg";
      string Port = "--port=";
      
      myId = Util.PID.ToString();
      driverUrl = null;
      logFile = null;
      launcherPort = 0;
      
      int argIndx = 0;
      while (argIndx <= (args.Length - 1) && args[argIndx].StartsWith("--"))
      {
        string arg = args[argIndx];
        if (arg.StartsWith(IDOption))
        {
          myId = arg.Substring(IDOption.Length);
        } 
        else if (arg.StartsWith(DriverOption))
        {
          driverUrl = arg.Substring(DriverOption.Length);
        }
        else if (arg.StartsWith(LogOption))
        {
          logFile = arg.Substring(LogOption.Length);
        }
        else if (arg == BGOption)
        {
          string procArgs = string.Empty;
          foreach (string newArg in args)
          {
            if (newArg != BGOption)
            {
              procArgs += '"' + newArg + "\" ";
            }
          }
          procArgs = procArgs.Trim();
          System.Diagnostics.Process bgProc;
          if (!Util.StartProcess(Environment.GetCommandLineArgs()[0],
            procArgs, false, null, false, false, false, true, out bgProc))
          {
            Util.Log("Failed to start background process with args: {0}",
              procArgs);
            Environment.Exit(1);
          }
          Environment.Exit(0);
        }
        else if (arg.StartsWith(StartDirOption))
        {
          string startDir = arg.Substring(StartDirOption.Length);
          if (startDir.Length > 0)
          {
            Environment.CurrentDirectory = startDir;
          }
        }
        else if (arg.StartsWith(Port))
        {
          string port = arg.Substring(Port.Length);
          try
          {
            launcherPort = int.Parse(port);
          }
          catch
          {
            Util.Log("Port number should be an integer: {0}", port);
            ShowUsage(args);
          }
        }
        else
        {
          Util.Log("Unknown option: {0}", arg);
          ShowUsage(args);
        }
        argIndx++;
      }
      if (args.Length != argIndx)
      {
        Util.Log("Incorrect number of arguments: {0}",
          (args.Length - argIndx));
        ShowUsage(args);
      }
      if (launcherPort == 0)
      {
        Util.Log("Port number is not specified.");
        ShowUsage(args);
      }
      
    }
  }

}
