-- MySQL dump 10.19  Distrib 10.3.39-MariaDB, for debian-linux-gnu (aarch64)
--
-- Host: localhost    Database: speaker_stream
-- ------------------------------------------------------
-- Server version	10.3.39-MariaDB-0ubuntu0.20.04.2

/*!40101 SET @OLD_CHARACTER_SET_CLIENT=@@CHARACTER_SET_CLIENT */;
/*!40101 SET @OLD_CHARACTER_SET_RESULTS=@@CHARACTER_SET_RESULTS */;
/*!40101 SET @OLD_COLLATION_CONNECTION=@@COLLATION_CONNECTION */;
/*!40101 SET NAMES utf8mb4 */;
/*!40103 SET @OLD_TIME_ZONE=@@TIME_ZONE */;
/*!40103 SET TIME_ZONE='+00:00' */;
/*!40014 SET @OLD_UNIQUE_CHECKS=@@UNIQUE_CHECKS, UNIQUE_CHECKS=0 */;
/*!40014 SET @OLD_FOREIGN_KEY_CHECKS=@@FOREIGN_KEY_CHECKS, FOREIGN_KEY_CHECKS=0 */;
/*!40101 SET @OLD_SQL_MODE=@@SQL_MODE, SQL_MODE='NO_AUTO_VALUE_ON_ZERO' */;
/*!40111 SET @OLD_SQL_NOTES=@@SQL_NOTES, SQL_NOTES=0 */;

--
-- Current Database: `speaker_stream`
--

CREATE DATABASE /*!32312 IF NOT EXISTS*/ `speaker_stream` /*!40100 DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci */;

USE `speaker_stream`;

--
-- Table structure for table `Device`
--

DROP TABLE IF EXISTS `Device`;
/*!40101 SET @saved_cs_client     = @@character_set_client */;
/*!40101 SET character_set_client = utf8 */;
CREATE TABLE `Device` (
  `device_uuid` char(36) NOT NULL,
  `auth_token_hash` binary(32) DEFAULT NULL,
  `status` varchar(16) NOT NULL,
  `member_id` bigint(20) unsigned DEFAULT NULL,
  `plan_name` varchar(50) NOT NULL,
  PRIMARY KEY (`device_uuid`),
  UNIQUE KEY `uq_device_uuid` (`device_uuid`),
  KEY `fk_device_member` (`member_id`),
  KEY `fk_device_plan_name` (`plan_name`),
  CONSTRAINT `fk_device_member` FOREIGN KEY (`member_id`) REFERENCES `Member` (`member_id`),
  CONSTRAINT `fk_device_plan_name` FOREIGN KEY (`plan_name`) REFERENCES `Plan` (`plan_name`),
  CONSTRAINT `chk_device_status` CHECK (`status` in ('ACTIVE','DISABLED'))
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
/*!40101 SET character_set_client = @saved_cs_client */;

--
-- Dumping data for table `Device`
--

LOCK TABLES `Device` WRITE;
/*!40000 ALTER TABLE `Device` DISABLE KEYS */;
/*!40000 ALTER TABLE `Device` ENABLE KEYS */;
UNLOCK TABLES;

--
-- Table structure for table `Member`
--

DROP TABLE IF EXISTS `Member`;
/*!40101 SET @saved_cs_client     = @@character_set_client */;
/*!40101 SET character_set_client = utf8 */;
CREATE TABLE `Member` (
  `member_id` bigint(20) unsigned NOT NULL AUTO_INCREMENT,
  `login_id` varchar(100) NOT NULL,
  `password_hash` varchar(255) NOT NULL,
  PRIMARY KEY (`member_id`),
  UNIQUE KEY `login_id` (`login_id`)
) ENGINE=InnoDB AUTO_INCREMENT=2 DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
/*!40101 SET character_set_client = @saved_cs_client */;

--
-- Dumping data for table `Member`
--

LOCK TABLES `Member` WRITE;
/*!40000 ALTER TABLE `Member` DISABLE KEYS */;
/*!40000 ALTER TABLE `Member` ENABLE KEYS */;
UNLOCK TABLES;

--
-- Table structure for table `Plan`
--

DROP TABLE IF EXISTS `Plan`;
/*!40101 SET @saved_cs_client     = @@character_set_client */;
/*!40101 SET character_set_client = utf8 */;
CREATE TABLE `Plan` (
  `plan_name` varchar(50) NOT NULL,
  `codec` varchar(16) NOT NULL,
  `bitrate_bps` int(11) NOT NULL,
  PRIMARY KEY (`plan_name`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
/*!40101 SET character_set_client = @saved_cs_client */;

--
-- Dumping data for table `Plan`
--

LOCK TABLES `Plan` WRITE;
/*!40000 ALTER TABLE `Plan` DISABLE KEYS */;
INSERT INTO `Plan` VALUES ('BASE','MP3',128000),('PREMIUM','MP3',320000);
/*!40000 ALTER TABLE `Plan` ENABLE KEYS */;
UNLOCK TABLES;

--
-- Table structure for table `Song`
--

DROP TABLE IF EXISTS `Song`;
/*!40101 SET @saved_cs_client     = @@character_set_client */;
/*!40101 SET character_set_client = utf8 */;
CREATE TABLE `Song` (
  `song_id` bigint(20) NOT NULL AUTO_INCREMENT,
  `title` varchar(200) NOT NULL,
  `file_path` varchar(500) NOT NULL,
  `codec` varchar(16) NOT NULL,
  `duration` bigint(20) NOT NULL,
  `file_size_bytes` bigint(20) NOT NULL,
  `checksum_sha256` char(64) NOT NULL,
  PRIMARY KEY (`song_id`),
  UNIQUE KEY `file_path` (`file_path`)
) ENGINE=InnoDB AUTO_INCREMENT=17 DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
/*!40101 SET character_set_client = @saved_cs_client */;

--
-- Dumping data for table `Song`
--

LOCK TABLES `Song` WRITE;
/*!40000 ALTER TABLE `Song` DISABLE KEYS */;
INSERT INTO `Song` VALUES (1,'BiiiG','/home/jetson/speaker_stream/base/BiiiG [ZBFzUSdS2nI].mp3','MP3',164,2628589,'dfa2828103b4605f0740d366e3fd5481b27002b7320d1c1f738ddaebda34f8e9'),(2,'I\'m gonna TOESA (Narr, KIAN84)','/home/jetson/speaker_stream/base/TOESA.mp3','MP3',211,3386349,'f398ada526c29f3f370bac55f69adcda5781a38e347ae2dd77c4e82a40338441'),(3,'Super Shy','/home/jetson/speaker_stream/base/Super Shy [0c7zGU2C2mM].mp3','MP3',154,2475616,'5853fb60d73e91382df7e99e75ccc57baf2714c60a3ca8ae0aecda25badfc196');
/*!40000 ALTER TABLE `Song` ENABLE KEYS */;
UNLOCK TABLES;
/*!40103 SET TIME_ZONE=@OLD_TIME_ZONE */;

/*!40101 SET SQL_MODE=@OLD_SQL_MODE */;
/*!40014 SET FOREIGN_KEY_CHECKS=@OLD_FOREIGN_KEY_CHECKS */;
/*!40014 SET UNIQUE_CHECKS=@OLD_UNIQUE_CHECKS */;
/*!40101 SET CHARACTER_SET_CLIENT=@OLD_CHARACTER_SET_CLIENT */;
/*!40101 SET CHARACTER_SET_RESULTS=@OLD_CHARACTER_SET_RESULTS */;
/*!40101 SET COLLATION_CONNECTION=@OLD_COLLATION_CONNECTION */;
/*!40111 SET SQL_NOTES=@OLD_SQL_NOTES */;

-- Dump completed on 2026-10-08  3:48:11
